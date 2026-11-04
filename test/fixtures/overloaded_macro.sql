CREATE OR REPLACE MACRO overloaded_macro(a) AS a, (a, b) AS a + b;
