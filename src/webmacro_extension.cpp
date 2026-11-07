#define DUCKDB_EXTENSION_MAIN

#include "webmacro_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/create_function_info.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_create.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

#include "query_farm_telemetry.hpp"

#ifdef __EMSCRIPTEN__
#include "wasm_httplib_replacement.hpp"
#else
#include "duckdb/main/http/http_util.hpp"
#endif

#include <mutex>

namespace duckdb {

static constexpr const char *WEBMACRO_VERSION = "2026110701";

static constexpr const char *ALLOW_HTTP_SETTING = "webmacro_allow_http";
static constexpr const char *MAX_RESPONSE_SIZE_SETTING = "webmacro_max_response_size";
static constexpr idx_t DEFAULT_MAX_RESPONSE_SIZE = 1024 * 1024; // 1 MiB
static constexpr idx_t MAX_REDIRECTS = 10;

enum class WebMacroScheme { HTTP, HTTPS };

//===--------------------------------------------------------------------===//
// URL validation and policy
//===--------------------------------------------------------------------===//

// Returns the scheme of an http(s) URL and rejects everything else (file://, s3://, bare paths, ...).
static WebMacroScheme ParseScheme(const string &url) {
	auto lower = StringUtil::Lower(url);
	idx_t prefix_len;
	WebMacroScheme scheme;
	if (StringUtil::StartsWith(lower, "https://")) {
		prefix_len = 8;
		scheme = WebMacroScheme::HTTPS;
	} else if (StringUtil::StartsWith(lower, "http://")) {
		prefix_len = 7;
		scheme = WebMacroScheme::HTTP;
	} else {
		throw InvalidInputException("load_macro_from_url: unsupported URL \"%s\" - only https:// URLs are supported",
		                            url);
	}
	auto host_end = url.find_first_of("/?#", prefix_len);
	auto authority = url.substr(prefix_len, host_end == string::npos ? string::npos : host_end - prefix_len);
	if (authority.empty()) {
		throw InvalidInputException("load_macro_from_url: URL \"%s\" has no host", url);
	}
	for (auto c : url) {
		if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) {
			throw InvalidInputException(
			    "load_macro_from_url: URL \"%s\" contains whitespace or control characters", url);
		}
	}
	return scheme;
}

static bool AllowPlainHttp(ClientContext &context) {
	Value value;
	if (context.TryGetCurrentSetting(ALLOW_HTTP_SETTING, value) && !value.IsNull()) {
		return BooleanValue::Get(value.DefaultCastAs(LogicalType::BOOLEAN));
	}
	return false;
}

static idx_t MaxResponseSize(ClientContext &context) {
	Value value;
	if (context.TryGetCurrentSetting(MAX_RESPONSE_SIZE_SETTING, value) && !value.IsNull()) {
		return UBigIntValue::Get(value.DefaultCastAs(LogicalType::UBIGINT));
	}
	return DEFAULT_MAX_RESPONSE_SIZE;
}

// Checks a URL (the one passed in, or a redirect target) against the extension's policy before it is fetched.
static void CheckUrlAllowed(ClientContext &context, const string &url) {
	auto scheme = ParseScheme(url);
	if (scheme == WebMacroScheme::HTTP && !AllowPlainHttp(context)) {
		throw PermissionException("load_macro_from_url: refusing to fetch \"%s\" over plain http:// - the response is "
		                          "executed as SQL, so it must come over https://. SET %s = true to allow plain "
		                          "HTTP at your own risk",
		                          url, ALLOW_HTTP_SETTING);
	}
	if (!Settings::Get<EnableExternalAccessSetting>(context)) {
		throw PermissionException("load_macro_from_url is disabled because enable_external_access is false");
	}
}

//===--------------------------------------------------------------------===//
// Fetching
//===--------------------------------------------------------------------===//

static string ResponseTooLargeMessage(const string &url, idx_t max_size) {
	return StringUtil::Format("load_macro_from_url: response from \"%s\" is larger than %s = %llu bytes", url,
	                          MAX_RESPONSE_SIZE_SETTING, max_size);
}

#ifndef __EMSCRIPTEN__

static bool IsRedirect(HTTPStatusCode status) {
	switch (status) {
	case HTTPStatusCode::MovedPermanently_301:
	case HTTPStatusCode::Found_302:
	case HTTPStatusCode::SeeOther_303:
	case HTTPStatusCode::TemporaryRedirect_307:
	case HTTPStatusCode::PermanentRedirect_308:
		return true;
	default:
		return false;
	}
}

// Resolves a Location header against the URL that returned it.
static string ResolveRedirect(const string &base_url, const string &location) {
	auto lower = StringUtil::Lower(location);
	if (StringUtil::StartsWith(lower, "http://") || StringUtil::StartsWith(lower, "https://")) {
		return location;
	}
	string path, proto_host_port;
	HTTPUtil::DecomposeURL(base_url, path, proto_host_port);
	if (StringUtil::StartsWith(location, "//")) {
		auto scheme_end = base_url.find("://");
		return base_url.substr(0, scheme_end + 1) + location;
	}
	if (StringUtil::StartsWith(location, "/")) {
		return proto_host_port + location;
	}
	// relative path: replace the last path segment
	auto query_pos = path.find_first_of("?#");
	auto dir = path.substr(0, query_pos);
	auto slash = dir.rfind('/');
	dir = slash == string::npos ? "/" : dir.substr(0, slash + 1);
	return proto_host_port + dir + location;
}

// DuckDB's built-in HTTP client has no TLS; https:// needs httpfs, which DuckDB autoloads for https:// paths too.
static HTTPUtil &GetHttpUtilFor(ClientContext &context, WebMacroScheme scheme) {
	auto &db = DatabaseInstance::GetDatabase(context);
	auto &http_util = HTTPUtil::Get(db);
	if (scheme != WebMacroScheme::HTTPS || http_util.GetName() != "Built-In") {
		return http_util;
	}
	ExtensionHelper::TryAutoLoadExtension(context, "httpfs");
	auto &loaded_util = HTTPUtil::Get(db);
	if (loaded_util.GetName() == "Built-In") {
		throw MissingExtensionException("load_macro_from_url needs the httpfs extension to fetch https:// URLs. Run "
		                                "INSTALL httpfs; LOAD httpfs; and try again");
	}
	return loaded_util;
}

// Fetches the URL through DuckDB's HTTP stack, so http_timeout, http_retries, http_proxy, CA settings and HTTP
// secrets (e.g. a bearer token for private gists) all apply. Redirects are followed by hand so that every hop is
// checked again, which keeps an https:// URL from being redirected to plain http://.
static string FetchUrl(ClientContext &context, const string &original_url) {
	const auto max_size = MaxResponseSize(context);
	string url = original_url;
	for (idx_t hop = 0; hop <= MAX_REDIRECTS; hop++) {
		CheckUrlAllowed(context, url);
		auto &http_util = GetHttpUtilFor(context, ParseScheme(url));
		auto params = http_util.InitializeParameters(context, url);
		params->follow_location = false;

		string body;
		bool too_large = false;
		bool interrupted = false;
		HTTPHeaders headers;
		GetRequestInfo request(
		    url, headers, *params,
		    [&](const HTTPResponse &response) {
			    body.clear();
			    if (too_large || interrupted) {
				    return false;
			    }
			    if (response.HasHeader("Content-Length")) {
				    idx_t content_length;
				    auto header = response.GetHeaderValue("Content-Length");
				    if (TryCast::Operation<string_t, idx_t>(string_t(header), content_length, false) &&
				        content_length > max_size) {
					    too_large = true;
					    return false;
				    }
			    }
			    return true;
		    },
		    [&](const_data_ptr_t data, idx_t data_length) {
			    if (context.IsInterrupted()) {
				    interrupted = true;
				    return false;
			    }
			    if (body.size() + data_length > max_size) {
				    too_large = true;
				    return false;
			    }
			    body.append(const_char_ptr_cast(data), data_length);
			    return true;
		    });

		auto response = http_util.Request(request);
		if (interrupted) {
			throw InterruptException();
		}
		if (too_large) {
			throw InvalidInputException(ResponseTooLargeMessage(url, max_size));
		}
		if (response->HasRequestError()) {
			throw IOException("load_macro_from_url: HTTP GET \"%s\" failed: %s", url, response->GetRequestError());
		}
		if (IsRedirect(response->status)) {
			if (!response->HasHeader("Location")) {
				throw HTTPException(StringUtil::Format("load_macro_from_url: HTTP %d redirect from \"%s\" has no "
				                                       "Location header",
				                                       static_cast<int>(response->status), url));
			}
			url = ResolveRedirect(url, response->GetHeaderValue("Location"));
			continue;
		}
		if (response->status != HTTPStatusCode::OK_200) {
			throw HTTPException(StringUtil::Format("load_macro_from_url: HTTP %d (%s) fetching \"%s\"",
			                                       static_cast<int>(response->status),
			                                       HTTPUtil::GetStatusMessage(response->status), url));
		}
		return body;
	}
	throw HTTPException(
	    StringUtil::Format("load_macro_from_url: too many redirects (more than %llu) fetching \"%s\"", MAX_REDIRECTS,
	                       original_url));
}

#else

// In the browser the request goes through XMLHttpRequest, so TLS, redirects and CORS are the browser's business.
static string FetchUrl(ClientContext &context, const string &url) {
	CheckUrlAllowed(context, url);
	auto max_size = MaxResponseSize(context);
	auto client_and_path = SetupHttpClient(url);
	auto res = client_and_path.first.Get(client_and_path.second);
	if (res->status != 200) {
		throw HTTPException(StringUtil::Format("load_macro_from_url: HTTP %d fetching \"%s\": %s",
		                                       static_cast<int>(res->status), url, res->reason));
	}
	if (res->body.size() > max_size) {
		throw InvalidInputException(ResponseTooLargeMessage(url, max_size));
	}
	return std::move(res->body);
}

#endif

//===--------------------------------------------------------------------===//
// Parsing and creating the macro
//===--------------------------------------------------------------------===//

// Parses the response with DuckDB's own parser and accepts exactly one CREATE MACRO statement. Everything else is
// rejected before anything runs, so a response can't carry extra statements alongside a macro.
static unique_ptr<SQLStatement> ParseMacroDefinition(const string &macro_sql, ClientContext &context) {
	Parser parser(context);
	try {
		parser.ParseQuery(macro_sql);
	} catch (std::exception &e) {
		throw InvalidInputException("load_macro_from_url: URL content is not valid SQL: %s", ErrorData(e).Message());
	}

	if (parser.statements.size() != 1) {
		throw InvalidInputException(
		    "load_macro_from_url: URL content must contain exactly one CREATE MACRO statement, found %llu statements",
		    static_cast<idx_t>(parser.statements.size()));
	}

	auto &statement = parser.statements[0];
	if (statement->type != StatementType::CREATE_STATEMENT) {
		throw InvalidInputException("load_macro_from_url: URL content must be a CREATE MACRO statement");
	}

	auto &info = *statement->Cast<CreateStatement>().info;
	if (info.type != CatalogType::MACRO_ENTRY && info.type != CatalogType::TABLE_MACRO_ENTRY) {
		throw InvalidInputException("load_macro_from_url: URL content must be a CREATE MACRO statement");
	}
	return std::move(statement);
}

// The binder and catalog are not meant to be driven from several threads of one query at once.
static std::mutex create_macro_lock;

// Creates the macro in the caller's own transaction, exactly as running the CREATE MACRO statement would: the
// caller's search path decides where an unqualified macro goes, TEMP macros are visible to the caller, and an
// explicit transaction that rolls back takes the macro with it.
static string CreateMacro(ClientContext &context, unique_ptr<SQLStatement> statement) {
	std::lock_guard<std::mutex> guard(create_macro_lock);

	auto binder = Binder::CreateBinder(context);
	auto bound = binder->Bind(*statement);
	if (!bound.plan || bound.plan->type != LogicalOperatorType::LOGICAL_CREATE_MACRO) {
		throw InternalException("load_macro_from_url: CREATE MACRO did not bind to a macro creation");
	}
	auto &create = bound.plan->Cast<LogicalCreate>();
	auto &info = create.info->Cast<CreateFunctionInfo>();

	// Same checks the client runs before executing a statement that writes to a database
	auto &meta_transaction = MetaTransaction::Get(context);
	auto &manager = DatabaseManager::Get(context);
	for (auto &it : binder->GetStatementProperties().modified_databases) {
		auto db = manager.GetDatabase(context, it.first);
		if (!db) {
			throw InvalidInputException("Database \"%s\" not found", it.first.GetIdentifierName());
		}
		if (db->IsReadOnly()) {
			throw InvalidInputException("load_macro_from_url: cannot create macro \"%s\" in database %s which is "
			                            "attached in read-only mode",
			                            info.GetFunctionName().GetIdentifierName(), it.first.GetIdentifierName());
		}
		meta_transaction.ModifyDatabase(*db, it.second.modifications);
	}

	auto &catalog = Catalog::GetCatalog(context, info.GetQualifiedName().Catalog());
	catalog.CreateFunction(context, info);
	return info.GetFunctionName().GetIdentifierName();
}

static void LoadMacroFromUrlFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();

	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, [&](string_t url_value) {
		auto url = url_value.GetString();
		auto macro_sql = FetchUrl(context, url);
		auto statement = ParseMacroDefinition(macro_sql, context);
		auto macro_name = CreateMacro(context, std::move(statement));
		return StringVector::AddString(result, "Successfully loaded macro: " + macro_name);
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(ALLOW_HTTP_SETTING,
	                          "Allow load_macro_from_url to fetch plain http:// URLs. Off by default: the response is "
	                          "executed as SQL, so anyone on the network path could replace the macro",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));
	config.AddExtensionOption(MAX_RESPONSE_SIZE_SETTING,
	                          "Largest response, in bytes, that load_macro_from_url will accept",
	                          LogicalType::UBIGINT, Value::UBIGINT(DEFAULT_MAX_RESPONSE_SIZE));

	ScalarFunction load_macro_from_url(
	    "load_macro_from_url",
	    {FunctionParameter("url", LogicalType::VARCHAR, {}, FunctionParameterKind::POSITIONAL_ONLY)},
	    LogicalType::VARCHAR, LoadMacroFromUrlFunction);
	load_macro_from_url.SetFallible();
	// Fetching and creating a catalog entry are side effects: never constant-fold or cache the call
	load_macro_from_url.SetVolatile();

	CreateScalarFunctionInfo info(load_macro_from_url);
	FunctionDescription description;
	description.description =
	    "Fetches a single CREATE MACRO statement from an https:// URL and creates the macro in the current "
	    "transaction. Returns a message naming the macro. Anything other than exactly one CREATE MACRO statement is "
	    "rejected before it runs. Refuses plain http:// unless webmacro_allow_http is set, and refuses entirely when "
	    "enable_external_access is false.";
	description.examples = {
	    "load_macro_from_url('https://raw.githubusercontent.com/Query-farm/sql-macros/main/macros/normalize_handle.sql')"};
	description.categories = {"macro", "network"};
	info.descriptions.push_back(std::move(description));
	loader.RegisterFunction(std::move(info));

	QueryFarmSendTelemetry(loader, "webmacro", WEBMACRO_VERSION);
}

void WebmacroExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string WebmacroExtension::Name() {
	return "webmacro";
}

std::string WebmacroExtension::Version() const {
	return WEBMACRO_VERSION;
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(webmacro, loader) {
	duckdb::LoadInternal(loader);
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
