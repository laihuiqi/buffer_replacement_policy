/* contrib/test_bufmgr/test_bufmgr--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_bufmgr" to load this file. \quit

--
-- test_bufmgr()
--
CREATE FUNCTION test_bufmgr(text, integer) RETURNS boolean
AS 'MODULE_PATHNAME', 'test_bufmgr'
LANGUAGE C STRICT;

