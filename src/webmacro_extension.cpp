#define DUCKDB_EXTENSION_MAIN

#include "webmacro_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/common/error_data.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/vector/string_vector.hpp"

#include "yyjson.hpp"
#include "query_farm_telemetry.hpp"

#ifdef __EMSCRIPTEN__
#include "wasm_httplib_replacement.hpp"
#else
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"
#endif

namespace duckdb
{

#ifndef __EMSCRIPTEN__
    // Helper function to setup HTTP client
    static std::pair<duckdb_httplib_openssl::Client, std::string> SetupHttpClient(const std::string &url)
    {
        std::string scheme, domain, path;
        size_t pos = url.find("://");
        std::string mod_url = url;
        if (pos != std::string::npos)
        {
            scheme = mod_url.substr(0, pos);
            mod_url.erase(0, pos + 3);
        }

        pos = mod_url.find("/");
        if (pos != std::string::npos)
        {
            domain = mod_url.substr(0, pos);
            path = mod_url.substr(pos);
        }
        else
        {
            domain = mod_url;
            path = "/";
        }

        duckdb_httplib_openssl::Client client(domain.c_str());
        client.set_read_timeout(10, 0);
        client.set_follow_location(true);

        return std::make_pair(std::move(client), path);
    }

    // Helper function to handle HTTP errors
    static void HandleHttpError(const duckdb_httplib_openssl::Result &res, const std::string &request_type)
    {
        std::string err_message = "HTTP " + request_type + " request failed. ";

        switch (res.error())
        {
        case duckdb_httplib_openssl::Error::Connection:
            err_message += "Connection error.";
            break;
        case duckdb_httplib_openssl::Error::Read:
            err_message += "Error reading response.";
            break;
        default:
            err_message += "Unknown error.";
            break;
        }
        throw std::runtime_error(err_message);
    }
#endif

    // Parses the response with DuckDB's own parser and accepts exactly one
    // CREATE MACRO statement. Everything else is rejected before anything runs,
    // so a response can't carry extra statements alongside a macro.
    static unique_ptr<SQLStatement> ParseMacroDefinition(const std::string &macro_sql, ClientContext &context,
                                                         std::string &macro_name)
    {
        Parser parser(context);
        try
        {
            parser.ParseQuery(macro_sql);
        }
        catch (std::exception &e)
        {
            throw std::runtime_error("URL content is not valid SQL: " + ErrorData(e).Message());
        }

        if (parser.statements.size() != 1)
        {
            throw std::runtime_error("URL content must contain exactly one CREATE MACRO statement, found " +
                                     std::to_string(parser.statements.size()) + " statements");
        }

        auto &statement = parser.statements[0];
        if (statement->type != StatementType::CREATE_STATEMENT)
        {
            throw std::runtime_error("URL content must be a CREATE MACRO statement");
        }

        auto &info = *statement->Cast<CreateStatement>().info;
        if (info.type != CatalogType::MACRO_ENTRY && info.type != CatalogType::TABLE_MACRO_ENTRY)
        {
            throw std::runtime_error("URL content must be a CREATE MACRO statement");
        }

        // The macro is created on a separate connection, so a temporary macro
        // would disappear as soon as that connection closes.
        if (info.temporary)
        {
            throw std::runtime_error("Temporary macros can't be loaded from a URL; remove TEMP from the definition");
        }

        macro_name = info.Cast<CreateFunctionInfo>().GetFunctionName().GetIdentifierName();
        return std::move(statement);
    }

    // Function to fetch and create macro from URL
    static void LoadMacroFromUrlFunction(DataChunk &args, ExpressionState &state, Vector &result, DatabaseInstance *db_instance)
    {
        auto &context = state.GetContext();

        UnaryExecutor::Execute<string_t, string_t>(
            args.data[0], result,
            [&](string_t url)
            {
                try
                {
                    // Setup HTTP client
                    auto client_and_path = SetupHttpClient(url.GetString());
                    auto &client = client_and_path.first;
                    auto &path = client_and_path.second;

                    // Make GET request
                    auto res = client.Get(path.c_str());
                    if (!res)
                    {
                        HandleHttpError(res, "GET");
                    }

                    if (res->status != 200)
                    {
                        throw HTTPException(std::to_string(res->status) + ": " + res->reason);
                    }

                    // Get the SQL content
                    std::string macro_sql = res->body;

                    std::string macro_name;
                    auto statement = ParseMacroDefinition(res->body, context, macro_name);

                    Connection conn(*db_instance);
                    auto query_result = conn.Query(std::move(statement));

                    if (query_result->HasError())
                    {
                        throw std::runtime_error("Failed loading Macro: " + query_result->GetError());
                    }

                    return StringVector::AddString(result, "Successfully loaded macro: " + macro_name);
                }
                catch (duckdb::HTTPException &e)
                {
                    throw;
                }
                catch (std::exception &e)
                {
                    std::string error_msg = "Error: " + std::string(e.what());
                    throw std::runtime_error(error_msg);
                }
            });
    }

    static void LoadInternal(ExtensionLoader &loader)
    {
        auto &instance = loader.GetDatabaseInstance();

        // Create lambda to capture database instance
        auto load_macro_func = [&instance](DataChunk &args, ExpressionState &state, Vector &result)
        {
            LoadMacroFromUrlFunction(args, state, result, &instance);
        };

        // Register function with captured database instance
        ScalarFunction load_macro_from_url("load_macro_from_url",
                                           {FunctionParameter("url", LogicalType::VARCHAR, {},
                                                              FunctionParameterKind::POSITIONAL_ONLY)},
                                           LogicalType::VARCHAR, load_macro_func);
        load_macro_from_url.SetFallible();
        loader.RegisterFunction(load_macro_from_url);

        QueryFarmSendTelemetry(loader, "webmacro", "2026110401");
    }

    void WebmacroExtension::Load(ExtensionLoader &loader)
    {
        LoadInternal(loader);
    }

    std::string WebmacroExtension::Name()
    {
        return "webmacro";
    }

    std::string WebmacroExtension::Version() const
    {
#ifdef EXT_VERSION_WEBMACRO
        return EXT_VERSION_WEBMACRO;
#else
        return "";
#endif
    }

} // namespace duckdb

extern "C"
{

    DUCKDB_CPP_EXTENSION_ENTRY(webmacro, loader)
    {
        duckdb::LoadInternal(loader);
    }
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
