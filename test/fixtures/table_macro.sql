CREATE OR REPLACE MACRO table_macro(n) AS TABLE SELECT i FROM range(n) t(i);
