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
 * Single source of truth DTO definitions for Yul AST JSON export, import, and JSON Schema.
 */

#pragma once

#include <rfl/DefaultVal.hpp>
#include <rfl/Literal.hpp>
#include <rfl/TaggedUnion.hpp>
#include <rfl/Variant.hpp>
#include <rfl/internal/has_reflector.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace solidity::yul::ast_json
{

/// Nullable value wrapper used with rfl::DefaultVal so serialization ALWAYS emits
/// `"field": null` when empty, while deserialization accepts omitted, `null`, or a value,
/// and JSON Schema emits a valid Draft 2020-12 nullable property with `"default": null`.
template <typename T>
struct Nullable
{
	std::optional<T> value;
};

struct YulExpression;
struct YulStatement;

struct YulIdentifier
{
	rfl::Literal<"YulIdentifier"> nodeType{};
	std::string src;
	std::optional<std::string> nativeSrc;
	std::string name;
};

struct YulTypedName
{
	rfl::Literal<"YulTypedName"> nodeType{};
	std::string src;
	std::optional<std::string> nativeSrc;
	std::string name;
	std::optional<std::string> type;
};

struct YulLiteral
{
	rfl::Literal<"YulLiteral"> nodeType{};
	std::string src;
	std::optional<std::string> nativeSrc;
	rfl::Literal<"number", "bool", "string"> kind{};
	std::optional<std::string> type;
	std::optional<std::string> value;
	std::optional<std::string> hexValue;
};

struct YulFunctionCall
{
	rfl::Literal<"YulFunctionCall"> nodeType{};
	std::string src;
	std::optional<std::string> nativeSrc;
	YulIdentifier functionName;
	std::vector<YulExpression> arguments;
};

struct YulExpression
{
	using ReflectionType = rfl::TaggedUnion<
		"nodeType",
		YulIdentifier,
		YulLiteral,
		YulFunctionCall
	>;

	ReflectionType value;

	ReflectionType const& reflection() const { return value; }
};

struct YulBlock
{
	rfl::Literal<"YulBlock"> nodeType{};
	std::string src;
	std::optional<std::string> nativeSrc;
	std::vector<YulStatement> statements;
};

struct YulBreak
{
	rfl::Literal<"YulBreak"> nodeType{};
	std::string src;
	std::optional<std::string> nativeSrc;
};

struct YulContinue
{
	rfl::Literal<"YulContinue"> nodeType{};
	std::string src;
	std::optional<std::string> nativeSrc;
};

struct YulLeave
{
	rfl::Literal<"YulLeave"> nodeType{};
	std::string src;
	std::optional<std::string> nativeSrc;
};

struct YulExpressionStatement
{
	rfl::Literal<"YulExpressionStatement"> nodeType{};
	std::string src;
	std::optional<std::string> nativeSrc;
	YulExpression expression;
};

struct YulAssignment
{
	rfl::Literal<"YulAssignment"> nodeType{};
	std::string src;
	std::optional<std::string> nativeSrc;
	std::optional<std::vector<YulIdentifier>> variableNames;
	rfl::DefaultVal<Nullable<YulExpression>> value{};
};

struct YulVariableDeclaration
{
	rfl::Literal<"YulVariableDeclaration"> nodeType{};
	std::string src;
	std::optional<std::string> nativeSrc;
	std::optional<std::vector<YulTypedName>> variables;
	rfl::DefaultVal<Nullable<YulExpression>> value{};
};

struct YulFunctionDefinition
{
	rfl::Literal<"YulFunctionDefinition"> nodeType{};
	std::string src;
	std::optional<std::string> nativeSrc;
	std::string name;
	std::optional<std::vector<YulTypedName>> parameters;
	std::optional<std::vector<YulTypedName>> returnVariables;
	YulBlock body;
};

struct YulIf
{
	rfl::Literal<"YulIf"> nodeType{};
	std::string src;
	std::optional<std::string> nativeSrc;
	YulExpression condition;
	YulBlock body;
};

struct YulCase
{
	rfl::Literal<"YulCase"> nodeType{};
	std::string src;
	std::optional<std::string> nativeSrc;
	rfl::Variant<YulLiteral, rfl::Literal<"default">> value;
	YulBlock body;
};

struct YulSwitch
{
	rfl::Literal<"YulSwitch"> nodeType{};
	std::string src;
	std::optional<std::string> nativeSrc;
	YulExpression expression;
	std::optional<std::vector<YulCase>> cases;
};

struct YulForLoop
{
	rfl::Literal<"YulForLoop"> nodeType{};
	std::string src;
	std::optional<std::string> nativeSrc;
	YulBlock pre;
	YulExpression condition;
	YulBlock post;
	YulBlock body;
};

struct YulStatement
{
	using ReflectionType = rfl::TaggedUnion<
		"nodeType",
		YulBreak,
		YulContinue,
		YulLeave,
		YulExpressionStatement,
		YulAssignment,
		YulVariableDeclaration,
		YulFunctionDefinition,
		YulIf,
		YulSwitch,
		YulForLoop,
		YulBlock
	>;

	ReflectionType value;

	ReflectionType const& reflection() const { return value; }
};

} // namespace solidity::yul::ast_json

namespace rfl
{

template <typename T>
struct Reflector<solidity::yul::ast_json::Nullable<T>>
{
	using ReflType = std::optional<T>;

	static solidity::yul::ast_json::Nullable<T> to(ReflType _value) noexcept
	{
		return solidity::yul::ast_json::Nullable<T>{std::move(_value)};
	}

	static ReflType const& from(solidity::yul::ast_json::Nullable<T> const& _nullable) noexcept
	{
		return _nullable.value;
	}
};

} // namespace rfl
