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
 * @date 2019
 * Converts inline assembly AST to JSON format
 */

#include <libyul/AsmJsonConverter.h>

#include <libyul/AST.h>
#include <libyul/AsmJsonSchema.h>
#include <libyul/Dialect.h>
#include <libyul/Exceptions.h>
#include <libyul/Utilities.h>
#include <libsolutil/CommonData.h>
#include <libsolutil/UTF8.h>

#include <rfl/NoExtraFields.hpp>
#include <rfl/VariantsAsOneOf.hpp>
#include <rfl/json/to_schema.hpp>
#include <rfl/nlohmann.hpp>

namespace solidity::yul
{

namespace
{

std::string formatSourceLocation(langutil::SourceLocation const& _location, std::optional<size_t> _sourceIndex)
{
	int const length = (_location.start >= 0 && _location.end >= 0 && _location.end >= _location.start)
		? _location.end - _location.start
		: -1;
	return std::to_string(_location.start) + ":" + std::to_string(length) + ":" +
		(_sourceIndex.has_value() ? std::to_string(_sourceIndex.value()) : "-1");
}

struct DtoBuilder
{
	Dialect const& dialect;
	std::optional<size_t> sourceIndex;

	template <typename Dto, typename Node>
	Dto initNode(Node const& _node) const
	{
		Dto dto{};
		dto.src = formatSourceLocation(originLocationOf(_node), sourceIndex);
		dto.nativeSrc = formatSourceLocation(nativeLocationOf(_node), sourceIndex);
		return dto;
	}

	ast_json::YulIdentifier toDto(Identifier const& _node) const
	{
		yulAssert(!_node.name.empty(), "Invalid identifier");
		auto dto = initNode<ast_json::YulIdentifier>(_node);
		dto.name = _node.name.str();
		return dto;
	}

	ast_json::YulIdentifier toDto(BuiltinName const& _node) const
	{
		// represents BuiltinName also with YulIdentifier node type to avoid a breaking change in the JSON interface
		auto dto = initNode<ast_json::YulIdentifier>(_node);
		dto.name = dialect.builtin(_node.handle).name;
		return dto;
	}

	ast_json::YulTypedName toDto(NameWithDebugData const& _node) const
	{
		yulAssert(!_node.name.empty(), "Invalid variable name.");
		auto dto = initNode<ast_json::YulTypedName>(_node);
		dto.name = _node.name.str();
		// even though types are removed from Yul, we keep this field in the Json interface to not introduce
		// a breaking change
		// can be removed with the next breaking version
		dto.type = "";
		return dto;
	}

	ast_json::YulLiteral toDto(Literal const& _node) const
	{
		yulAssert(validLiteral(_node));
		auto dto = initNode<ast_json::YulLiteral>(_node);
		switch (_node.kind)
		{
		case LiteralKind::Number:
			dto.kind = rfl::Literal<"number", "bool", "string">::make<"number">();
			break;
		case LiteralKind::Boolean:
			dto.kind = rfl::Literal<"number", "bool", "string">::make<"bool">();
			break;
		case LiteralKind::String:
			dto.kind = rfl::Literal<"number", "bool", "string">::make<"string">();
			dto.hexValue = util::toHex(util::asBytes(formatLiteral(_node)));
			break;
		}
		dto.type = "";
		if (auto const formattedLiteral = formatLiteral(_node); util::validateUTF8(formattedLiteral))
			dto.value = formattedLiteral;
		return dto;
	}

	ast_json::YulExpression toExpressionDto(Expression const& _expr) const
	{
		return std::visit(
			[this](auto const& _node) -> ast_json::YulExpression { return ast_json::YulExpression{toDto(_node)}; },
			_expr
		);
	}

	ast_json::YulStatement toStatementDto(Statement const& _stmt) const
	{
		return std::visit(
			[this](auto const& _node) -> ast_json::YulStatement { return ast_json::YulStatement{toDto(_node)}; },
			_stmt
		);
	}

	ast_json::YulFunctionCall toDto(FunctionCall const& _node) const
	{
		auto dto = initNode<ast_json::YulFunctionCall>(_node);
		dto.functionName = std::visit([this](auto const& _fn) { return toDto(_fn); }, _node.functionName);
		dto.arguments.reserve(_node.arguments.size());
		for (auto const& arg: _node.arguments)
			dto.arguments.emplace_back(toExpressionDto(arg));
		return dto;
	}

	ast_json::YulBlock toDto(Block const& _node) const
	{
		auto dto = initNode<ast_json::YulBlock>(_node);
		dto.statements.reserve(_node.statements.size());
		for (auto const& stmt: _node.statements)
			dto.statements.emplace_back(toStatementDto(stmt));
		return dto;
	}

	ast_json::YulExpressionStatement toDto(ExpressionStatement const& _node) const
	{
		auto dto = initNode<ast_json::YulExpressionStatement>(_node);
		dto.expression = toExpressionDto(_node.expression);
		return dto;
	}

	ast_json::YulAssignment toDto(Assignment const& _node) const
	{
		yulAssert(_node.variableNames.size() >= 1, "Invalid assignment syntax");
		auto dto = initNode<ast_json::YulAssignment>(_node);
		std::vector<ast_json::YulIdentifier> vars;
		vars.reserve(_node.variableNames.size());
		for (auto const& var: _node.variableNames)
			vars.emplace_back(toDto(var));
		dto.variableNames = std::move(vars);
		if (_node.value)
			dto.value = ast_json::Nullable<ast_json::YulExpression>{toExpressionDto(*_node.value)};
		return dto;
	}

	ast_json::YulVariableDeclaration toDto(VariableDeclaration const& _node) const
	{
		auto dto = initNode<ast_json::YulVariableDeclaration>(_node);
		if (!_node.variables.empty())
		{
			std::vector<ast_json::YulTypedName> vars;
			vars.reserve(_node.variables.size());
			for (auto const& var: _node.variables)
				vars.emplace_back(toDto(var));
			dto.variables = std::move(vars);
		}
		if (_node.value)
			dto.value = ast_json::Nullable<ast_json::YulExpression>{toExpressionDto(*_node.value)};
		return dto;
	}

	ast_json::YulFunctionDefinition toDto(FunctionDefinition const& _node) const
	{
		yulAssert(!_node.name.empty(), "Invalid function name.");
		auto dto = initNode<ast_json::YulFunctionDefinition>(_node);
		dto.name = _node.name.str();
		if (!_node.parameters.empty())
		{
			std::vector<ast_json::YulTypedName> params;
			params.reserve(_node.parameters.size());
			for (auto const& var: _node.parameters)
				params.emplace_back(toDto(var));
			dto.parameters = std::move(params);
		}
		if (!_node.returnVariables.empty())
		{
			std::vector<ast_json::YulTypedName> retVars;
			retVars.reserve(_node.returnVariables.size());
			for (auto const& var: _node.returnVariables)
				retVars.emplace_back(toDto(var));
			dto.returnVariables = std::move(retVars);
		}
		dto.body = toDto(_node.body);
		return dto;
	}

	ast_json::YulIf toDto(If const& _node) const
	{
		yulAssert(_node.condition, "Invalid if condition.");
		auto dto = initNode<ast_json::YulIf>(_node);
		dto.condition = toExpressionDto(*_node.condition);
		dto.body = toDto(_node.body);
		return dto;
	}

	ast_json::YulCase toDto(Case const& _node) const
	{
		auto dto = initNode<ast_json::YulCase>(_node);
		if (_node.value)
			dto.value = toDto(*_node.value);
		else
			dto.value = rfl::Literal<"default">{};
		dto.body = toDto(_node.body);
		return dto;
	}

	ast_json::YulSwitch toDto(Switch const& _node) const
	{
		yulAssert(_node.expression, "Invalid expression pointer.");
		auto dto = initNode<ast_json::YulSwitch>(_node);
		dto.expression = toExpressionDto(*_node.expression);
		if (!_node.cases.empty())
		{
			std::vector<ast_json::YulCase> cases;
			cases.reserve(_node.cases.size());
			for (auto const& c: _node.cases)
				cases.emplace_back(toDto(c));
			dto.cases = std::move(cases);
		}
		return dto;
	}

	ast_json::YulForLoop toDto(ForLoop const& _node) const
	{
		yulAssert(_node.condition, "Invalid for loop condition.");
		auto dto = initNode<ast_json::YulForLoop>(_node);
		dto.pre = toDto(_node.pre);
		dto.condition = toExpressionDto(*_node.condition);
		dto.post = toDto(_node.post);
		dto.body = toDto(_node.body);
		return dto;
	}

	ast_json::YulBreak toDto(Break const& _node) const
	{
		return initNode<ast_json::YulBreak>(_node);
	}

	ast_json::YulContinue toDto(Continue const& _node) const
	{
		return initNode<ast_json::YulContinue>(_node);
	}

	ast_json::YulLeave toDto(Leave const& _node) const
	{
		return initNode<ast_json::YulLeave>(_node);
	}
};

} // anonymous namespace

Json AsmJsonConverter::operator()(Block const& _node) const { return rfl::nlohmann::write(DtoBuilder{m_dialect, m_sourceIndex}.toDto(_node)); }
Json AsmJsonConverter::operator()(NameWithDebugData const& _node) const { return rfl::nlohmann::write(DtoBuilder{m_dialect, m_sourceIndex}.toDto(_node)); }
Json AsmJsonConverter::operator()(Literal const& _node) const { return rfl::nlohmann::write(DtoBuilder{m_dialect, m_sourceIndex}.toDto(_node)); }
Json AsmJsonConverter::operator()(Identifier const& _node) const { return rfl::nlohmann::write(DtoBuilder{m_dialect, m_sourceIndex}.toDto(_node)); }
Json AsmJsonConverter::operator()(BuiltinName const& _node) const { return rfl::nlohmann::write(DtoBuilder{m_dialect, m_sourceIndex}.toDto(_node)); }
Json AsmJsonConverter::operator()(Assignment const& _node) const { return rfl::nlohmann::write(DtoBuilder{m_dialect, m_sourceIndex}.toDto(_node)); }
Json AsmJsonConverter::operator()(FunctionCall const& _node) const { return rfl::nlohmann::write(DtoBuilder{m_dialect, m_sourceIndex}.toDto(_node)); }
Json AsmJsonConverter::operator()(ExpressionStatement const& _node) const { return rfl::nlohmann::write(DtoBuilder{m_dialect, m_sourceIndex}.toDto(_node)); }
Json AsmJsonConverter::operator()(VariableDeclaration const& _node) const { return rfl::nlohmann::write(DtoBuilder{m_dialect, m_sourceIndex}.toDto(_node)); }
Json AsmJsonConverter::operator()(FunctionDefinition const& _node) const { return rfl::nlohmann::write(DtoBuilder{m_dialect, m_sourceIndex}.toDto(_node)); }
Json AsmJsonConverter::operator()(If const& _node) const { return rfl::nlohmann::write(DtoBuilder{m_dialect, m_sourceIndex}.toDto(_node)); }
Json AsmJsonConverter::operator()(Switch const& _node) const { return rfl::nlohmann::write(DtoBuilder{m_dialect, m_sourceIndex}.toDto(_node)); }
Json AsmJsonConverter::operator()(Case const& _node) const { return rfl::nlohmann::write(DtoBuilder{m_dialect, m_sourceIndex}.toDto(_node)); }
Json AsmJsonConverter::operator()(ForLoop const& _node) const { return rfl::nlohmann::write(DtoBuilder{m_dialect, m_sourceIndex}.toDto(_node)); }
Json AsmJsonConverter::operator()(Break const& _node) const { return rfl::nlohmann::write(DtoBuilder{m_dialect, m_sourceIndex}.toDto(_node)); }
Json AsmJsonConverter::operator()(Continue const& _node) const { return rfl::nlohmann::write(DtoBuilder{m_dialect, m_sourceIndex}.toDto(_node)); }
Json AsmJsonConverter::operator()(Leave const& _node) const { return rfl::nlohmann::write(DtoBuilder{m_dialect, m_sourceIndex}.toDto(_node)); }

std::string AsmJsonConverter::astJsonSchema(util::JsonFormat const& _format)
{
	std::string raw = rfl::json::to_schema<ast_json::YulBlock, rfl::NoExtraFields, rfl::VariantsAsOneOf>(
		0,
		"Solidity Yul AST JSON schema (generated from yul::ast_json::YulBlock)"
	);
	if (_format.format == util::JsonFormat::Pretty)
		return util::jsonPrint(Json::parse(raw), _format);
	return raw;
}

}
