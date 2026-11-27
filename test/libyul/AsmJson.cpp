/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0
/**
 * Unit tests for symmetric Yul AST JSON export/import and schema generation.
 */

#include <libyul/AST.h>
#include <libyul/AsmJsonConverter.h>
#include <libyul/AsmJsonImporter.h>
#include <libyul/AsmParser.h>
#include <libyul/AsmPrinter.h>
#include <libyul/Exceptions.h>
#include <libyul/backends/evm/EVMDialect.h>

#include <liblangutil/CharStream.h>
#include <liblangutil/EVMVersion.h>
#include <liblangutil/ErrorReporter.h>

#include <libsolutil/JSON.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace solidity;
using namespace solidity::langutil;
using namespace solidity::yul;

namespace solidity::yul::test
{

namespace
{

EVMDialect const& evmDialect()
{
	return EVMDialect::strictAssemblyForEVM(EVMVersion{});
}

std::shared_ptr<AST> parseYulBlock(std::string const& _source)
{
	ErrorList errors;
	ErrorReporter reporter(errors);
	CharStream stream(_source, "test.yul");
	auto ast = Parser(reporter, evmDialect()).parse(stream);
	BOOST_REQUIRE(ast != nullptr);
	BOOST_REQUIRE(errors.empty());
	return ast;
}

} // anonymous namespace

BOOST_AUTO_TEST_SUITE(YulAsmJson)

BOOST_AUTO_TEST_CASE(roundtrip_all_node_types)
{
	std::string const source = R"({
		function addPair(a, b) -> sum, diff {
			sum := add(a, b)
			diff := sub(a, b)
			if iszero(sum) { leave }
		}
		function noop() {}
		let uninit
		let x := 42
		let flag := true
		let greeting := "hello"
		let raw := hex"deadbeef"
		let s, d := addPair(x, 1)
		s, d := addPair(s, d)
		noop()
		{
			switch s
			case 0 { pop(0) }
			case 1 { pop(1) }
			default { pop(2) }
		}
		for { let i := 0 } lt(i, 3) { i := add(i, 1) } {
			if eq(i, 1) { continue }
			if eq(i, 2) { break }
		}
	})";

	auto const ast = parseYulBlock(source);
	auto const& dialect = evmDialect();
	Json const exported = AsmJsonConverter(dialect, 0)(ast->root());

	std::vector<std::shared_ptr<std::string const>> const sourceNames{
		std::make_shared<std::string const>("test.yul")
	};
	AST const imported = AsmJsonImporter(dialect, sourceNames).createAST(exported);
	Json const reExported = AsmJsonConverter(dialect, 0)(imported.root());

	BOOST_CHECK_EQUAL(exported.dump(), reExported.dump());
	BOOST_CHECK_EQUAL(
		AsmPrinter(dialect)(ast->root()),
		AsmPrinter(dialect)(imported.root())
	);
}

BOOST_AUTO_TEST_CASE(wire_format_quirks)
{
	std::string const source = R"({
		function noop() {}
		let uninit
		let utf8Str := "abc"
		let nonUtf8 := hex"fffe"
		noop()
	})";

	auto const ast = parseYulBlock(source);
	auto const& dialect = evmDialect();
	Json exported = AsmJsonConverter(dialect, 0)(ast->root());

	// YulFunctionDefinition with no parameters/returns omits those keys.
	Json const& fnDef = exported["statements"][0];
	BOOST_TEST(fnDef["nodeType"] == "YulFunctionDefinition");
	BOOST_TEST(!fnDef.contains("parameters"));
	BOOST_TEST(!fnDef.contains("returnVariables"));
	BOOST_TEST(fnDef["body"]["statements"].is_array());
	BOOST_TEST(fnDef["body"]["statements"].empty());

	// YulVariableDeclaration without initializer emits explicit `"value": null`.
	Json const& uninitDecl = exported["statements"][1];
	BOOST_TEST(uninitDecl["nodeType"] == "YulVariableDeclaration");
	BOOST_REQUIRE(uninitDecl.contains("value"));
	BOOST_TEST(uninitDecl["value"].is_null());

	// YulLiteral string quirks: UTF-8 has value+hexValue; non-UTF-8 has hexValue only.
	Json const& utf8Lit = exported["statements"][2]["value"];
	BOOST_TEST(utf8Lit["kind"] == "string");
	BOOST_TEST(utf8Lit["type"] == "");
	BOOST_TEST(utf8Lit["value"] == "abc");
	BOOST_TEST(utf8Lit["hexValue"] == "616263");

	Json const& nonUtf8Lit = exported["statements"][3]["value"];
	BOOST_TEST(nonUtf8Lit["kind"] == "string");
	BOOST_TEST(nonUtf8Lit["type"] == "");
	BOOST_TEST(!nonUtf8Lit.contains("value"));
	BOOST_TEST(nonUtf8Lit["hexValue"] == "fffe");

	// YulFunctionCall with 0 args still emits `"arguments": []`.
	Json const& callExpr = exported["statements"][4]["expression"];
	BOOST_TEST(callExpr["nodeType"] == "YulFunctionCall");
	BOOST_TEST(callExpr["arguments"].is_array());
	BOOST_TEST(callExpr["arguments"].empty());

	// Importer accepts omitted `"value"` (as produced after removeNullMembers) and omitted `nativeSrc`.
	Json stripped = util::removeNullMembers(exported);
	BOOST_TEST(!stripped["statements"][1].contains("value"));
	stripped.erase("nativeSrc");

	std::vector<std::shared_ptr<std::string const>> const sourceNames{
		std::make_shared<std::string const>("test.yul")
	};
	AST const imported = AsmJsonImporter(dialect, sourceNames).createAST(stripped);
	Json const reExported = AsmJsonConverter(dialect, 0)(imported.root());
	BOOST_CHECK_EQUAL(exported.dump(), reExported.dump());
}

BOOST_AUTO_TEST_CASE(ast_json_schema_structure_and_recursion)
{
	std::string const raw = AsmJsonConverter::astJsonSchema();
	Json schema;
	BOOST_REQUIRE(util::jsonParseStrict(raw, schema));
	BOOST_TEST(schema["$schema"] == "https://json-schema.org/draft/2020-12/schema");
	BOOST_TEST(schema["$comment"].is_string());
	BOOST_REQUIRE(schema.contains("$defs") && schema["$defs"].is_object());
	Json const& defs = schema["$defs"];

	// Ensure zero synthetic `__tagged` wrapper definitions exist.
	for (auto const& [key, val]: defs.items())
		BOOST_TEST(key.find("__tagged") == std::string::npos);

	auto findDef = [&](std::string const& _suffix) -> Json const& {
		for (auto const& [key, val]: defs.items())
			if (key.ends_with("__" + _suffix) || key == _suffix)
				return val;
		BOOST_FAIL("Missing $defs entry for " + _suffix);
		return defs;
	};

	// Recursive unions emit clean oneOf with exact arity.
	Json const& exprDef = findDef("YulExpression");
	BOOST_REQUIRE(exprDef.contains("oneOf") && exprDef["oneOf"].is_array());
	BOOST_TEST(exprDef["oneOf"].size() == 3u);

	Json const& stmtDef = findDef("YulStatement");
	BOOST_REQUIRE(stmtDef.contains("oneOf") && stmtDef["oneOf"].is_array());
	BOOST_TEST(stmtDef["oneOf"].size() == 11u);

	// YulCase::value uses oneOf (YulLiteral | "default").
	Json const& caseDef = findDef("YulCase");
	BOOST_TEST(caseDef["additionalProperties"] == false);
	BOOST_REQUIRE(caseDef["properties"]["value"].contains("oneOf"));
	BOOST_TEST(caseDef["properties"]["value"]["oneOf"].size() == 2u);

	// YulVariableDeclaration::value has default null and is not required.
	Json const& varDeclDef = findDef("YulVariableDeclaration");
	BOOST_TEST(varDeclDef["additionalProperties"] == false);
	BOOST_REQUIRE(varDeclDef["properties"]["value"].contains("default"));
	BOOST_TEST(varDeclDef["properties"]["value"]["default"].is_null());
	auto const required = varDeclDef["required"].get<std::set<std::string>>();
	BOOST_TEST(!required.count("value"));

	// Verify all 15 Yul node types are present with additionalProperties: false.
	for (std::string const& name: {
		"YulBlock", "YulIdentifier", "YulTypedName", "YulLiteral", "YulFunctionCall",
		"YulExpressionStatement", "YulAssignment", "YulVariableDeclaration",
		"YulFunctionDefinition", "YulIf", "YulSwitch", "YulCase",
		"YulForLoop", "YulBreak", "YulContinue", "YulLeave"
	})
	{
		Json const& def = findDef(name);
		BOOST_TEST(def["type"] == "object");
		BOOST_TEST(def["additionalProperties"] == false);
	}

	// Pretty-printed schema parses identically.
	std::string const pretty = AsmJsonConverter::astJsonSchema(util::JsonFormat{util::JsonFormat::Pretty, 2});
	BOOST_TEST(pretty.find('\n') != std::string::npos);
	Json parsedPretty;
	BOOST_REQUIRE(util::jsonParseStrict(pretty, parsedPretty));
	BOOST_TEST(parsedPretty == schema);
}

BOOST_AUTO_TEST_CASE(negative_import_assertions)
{
	auto const& dialect = evmDialect();
	std::vector<std::shared_ptr<std::string const>> const sourceNames{
		std::make_shared<std::string const>("test.yul")
	};
	AsmJsonImporter importer(dialect, sourceNames);

	auto makeBlock = [](Json _stmt) {
		return Json{
			{"nodeType", "YulBlock"},
			{"src", "0:10:0"},
			{"statements", Json::array({std::move(_stmt)})}
		};
	};

	// Invalid statement nodeType
	BOOST_CHECK_THROW(
		importer.createAST(makeBlock(Json{{"nodeType", "YulUnknown"}, {"src", "0:1:0"}})),
		YulAssertion
	);

	// Invalid source location (source index out of bounds / empty)
	BOOST_CHECK_THROW(
		importer.createAST(Json{{"nodeType", "YulBlock"}, {"src", "-1:-1:-1"}, {"statements", Json::array()}}),
		YulAssertion
	);

	// Non-empty literal type rejected
	BOOST_CHECK_THROW(
		importer.createAST(makeBlock(Json{
			{"nodeType", "YulExpressionStatement"},
			{"src", "0:5:0"},
			{"expression", Json{
				{"nodeType", "YulLiteral"},
				{"src", "0:1:0"},
				{"kind", "number"},
				{"type", "u256"},
				{"value", "1"}
			}}
		})),
		YulAssertion
	);

	// Invalid literal kind rejected
	BOOST_CHECK_THROW(
		importer.createAST(makeBlock(Json{
			{"nodeType", "YulExpressionStatement"},
			{"src", "0:5:0"},
			{"expression", Json{
				{"nodeType", "YulLiteral"},
				{"src", "0:1:0"},
				{"kind", "bytes"},
				{"value", "1"}
			}}
		})),
		YulAssertion
	);

	// String literal > 32 bytes rejected
	BOOST_CHECK_THROW(
		importer.createAST(makeBlock(Json{
			{"nodeType", "YulExpressionStatement"},
			{"src", "0:5:0"},
			{"expression", Json{
				{"nodeType", "YulLiteral"},
				{"src", "0:35:0"},
				{"kind", "string"},
				{"value", std::string(33, 'a')}
			}}
		})),
		YulAssertion
	);

	// YulAssignment with null value rejected
	BOOST_CHECK_THROW(
		importer.createAST(makeBlock(Json{
			{"nodeType", "YulAssignment"},
			{"src", "0:5:0"},
			{"variableNames", Json::array({Json{{"nodeType", "YulIdentifier"}, {"src", "0:1:0"}, {"name", "x"}}})},
			{"value", nullptr}
		})),
		YulAssertion
	);

	// YulCase with invalid string value (not "default") rejected
	BOOST_CHECK_THROW(
		importer.createAST(makeBlock(Json{
			{"nodeType", "YulSwitch"},
			{"src", "0:10:0"},
			{"expression", Json{{"nodeType", "YulIdentifier"}, {"src", "0:1:0"}, {"name", "x"}}},
			{"cases", Json::array({Json{
				{"nodeType", "YulCase"},
				{"src", "0:5:0"},
				{"value", "other"},
				{"body", Json{{"nodeType", "YulBlock"}, {"src", "0:2:0"}, {"statements", Json::array()}}}
			}})}
		})),
		YulAssertion
	);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace solidity::yul::test
