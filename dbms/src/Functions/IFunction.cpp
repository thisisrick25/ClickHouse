#include <Common/config.h>
#include <Common/typeid_cast.h>
#include <Columns/ColumnConst.h>
#include <Columns/ColumnNullable.h>
#include <DataTypes/DataTypeNothing.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/Native.h>
#include <DataTypes/DataTypeWithDictionary.h>
#include <DataTypes/getLeastSupertype.h>
#include <Columns/ColumnWithDictionary.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/IFunction.h>
#include <Interpreters/ExpressionActions.h>
#include <IO/WriteHelpers.h>
#include <ext/range.h>
#include <ext/collection_cast.h>
#include <cstdlib>
#include <memory>
#include <optional>

#if USE_EMBEDDED_COMPILER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <llvm/IR/IRBuilder.h> // Y_IGNORE
#pragma GCC diagnostic pop
#endif


namespace DB
{

namespace ErrorCodes
{
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int ILLEGAL_COLUMN;
}


namespace
{


/** Return ColumnNullable of src, with null map as OR-ed null maps of args columns in blocks.
  * Or ColumnConst(ColumnNullable) if the result is always NULL or if the result is constant and always not NULL.
  */
ColumnPtr wrapInNullable(const ColumnPtr & src, Block & block, const ColumnNumbers & args, size_t result, size_t input_rows_count)
{
    ColumnPtr result_null_map_column;

    /// If result is already nullable.
    ColumnPtr src_not_nullable = src;

    if (src->onlyNull())
        return src;
    else if (src->isColumnNullable())
    {
        src_not_nullable = static_cast<const ColumnNullable &>(*src).getNestedColumnPtr();
        result_null_map_column = static_cast<const ColumnNullable &>(*src).getNullMapColumnPtr();
    }

    for (const auto & arg : args)
    {
        const ColumnWithTypeAndName & elem = block.getByPosition(arg);
        if (!elem.type->isNullable())
            continue;

        /// Const Nullable that are NULL.
        if (elem.column->onlyNull())
            return block.getByPosition(result).type->createColumnConst(input_rows_count, Null());

        if (elem.column->isColumnConst())
            continue;

        if (elem.column->isColumnNullable())
        {
            const ColumnPtr & null_map_column = static_cast<const ColumnNullable &>(*elem.column).getNullMapColumnPtr();
            if (!result_null_map_column)
            {
                result_null_map_column = null_map_column;
            }
            else
            {
                MutableColumnPtr mutable_result_null_map_column = (*std::move(result_null_map_column)).mutate();

                NullMap & result_null_map = static_cast<ColumnUInt8 &>(*mutable_result_null_map_column).getData();
                const NullMap & src_null_map = static_cast<const ColumnUInt8 &>(*null_map_column).getData();

                for (size_t i = 0, size = result_null_map.size(); i < size; ++i)
                    if (src_null_map[i])
                        result_null_map[i] = 1;

                result_null_map_column = std::move(mutable_result_null_map_column);
            }
        }
    }

    if (!result_null_map_column)
        return makeNullable(src);

    if (src_not_nullable->isColumnConst())
        return ColumnNullable::create(src_not_nullable->convertToFullColumnIfConst(), result_null_map_column);
    else
        return ColumnNullable::create(src_not_nullable, result_null_map_column);
}


struct NullPresence
{
    bool has_nullable = false;
    bool has_null_constant = false;
};

NullPresence getNullPresense(const Block & block, const ColumnNumbers & args)
{
    NullPresence res;

    for (const auto & arg : args)
    {
        const auto & elem = block.getByPosition(arg);

        if (!res.has_nullable)
            res.has_nullable = elem.type->isNullable();
        if (!res.has_null_constant)
            res.has_null_constant = elem.type->onlyNull();
    }

    return res;
}

NullPresence getNullPresense(const ColumnsWithTypeAndName & args)
{
    NullPresence res;

    for (const auto & elem : args)
    {
        if (!res.has_nullable)
            res.has_nullable = elem.type->isNullable();
        if (!res.has_null_constant)
            res.has_null_constant = elem.type->onlyNull();
    }

    return res;
}

bool allArgumentsAreConstants(const Block & block, const ColumnNumbers & args)
{
    for (auto arg : args)
        if (!block.getByPosition(arg).column->isColumnConst())
            return false;
    return true;
}
}

bool PreparedFunctionImpl::defaultImplementationForConstantArguments(Block & block, const ColumnNumbers & args, size_t result,
                                                                     size_t input_rows_count)
{
    ColumnNumbers arguments_to_remain_constants = getArgumentsThatAreAlwaysConstant();

    /// Check that these arguments are really constant.
    for (auto arg_num : arguments_to_remain_constants)
        if (arg_num < args.size() && !block.getByPosition(args[arg_num]).column->isColumnConst())
            throw Exception("Argument at index " + toString(arg_num) + " for function " + getName() + " must be constant", ErrorCodes::ILLEGAL_COLUMN);

    if (args.empty() || !useDefaultImplementationForConstants() || !allArgumentsAreConstants(block, args))
        return false;

    Block temporary_block;
    bool have_converted_columns = false;

    size_t arguments_size = args.size();
    for (size_t arg_num = 0; arg_num < arguments_size; ++arg_num)
    {
        const ColumnWithTypeAndName & column = block.getByPosition(args[arg_num]);

        if (arguments_to_remain_constants.end() != std::find(arguments_to_remain_constants.begin(), arguments_to_remain_constants.end(), arg_num))
            temporary_block.insert(column);
        else
        {
            have_converted_columns = true;
            temporary_block.insert({ static_cast<const ColumnConst *>(column.column.get())->getDataColumnPtr(), column.type, column.name });
        }
    }

    /** When using default implementation for constants, the function requires at least one argument
      *  not in "arguments_to_remain_constants" set. Otherwise we get infinite recursion.
      */
    if (!have_converted_columns)
        throw Exception("Number of arguments for function " + getName() + " doesn't match: the function requires more arguments",
            ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

    temporary_block.insert(block.getByPosition(result));

    ColumnNumbers temporary_argument_numbers(arguments_size);
    for (size_t i = 0; i < arguments_size; ++i)
        temporary_argument_numbers[i] = i;

    executeWithoutColumnsWithDictionary(temporary_block, temporary_argument_numbers, arguments_size, temporary_block.rows());

    block.getByPosition(result).column = ColumnConst::create(temporary_block.getByPosition(arguments_size).column, input_rows_count);
    return true;
}


bool PreparedFunctionImpl::defaultImplementationForNulls(Block & block, const ColumnNumbers & args, size_t result,
                                                         size_t input_rows_count)
{
    if (args.empty() || !useDefaultImplementationForNulls())
        return false;

    NullPresence null_presence = getNullPresense(block, args);

    if (null_presence.has_null_constant)
    {
        block.getByPosition(result).column = block.getByPosition(result).type->createColumnConst(input_rows_count, Null());
        return true;
    }

    if (null_presence.has_nullable)
    {
        Block temporary_block = createBlockWithNestedColumns(block, args, result);
        executeWithoutColumnsWithDictionary(temporary_block, args, result, temporary_block.rows());
        block.getByPosition(result).column = wrapInNullable(temporary_block.getByPosition(result).column, block, args,
                                                            result, input_rows_count);
        return true;
    }

    return false;
}

void PreparedFunctionImpl::executeWithoutColumnsWithDictionary(Block & block, const ColumnNumbers & args, size_t result, size_t input_rows_count)
{
    if (defaultImplementationForConstantArguments(block, args, result, input_rows_count))
        return;

    if (defaultImplementationForNulls(block, args, result, input_rows_count))
        return;

    executeImpl(block, args, result, input_rows_count);
}

static Block removeColumnsWithDictionary(Block & block, const ColumnNumbers & args, size_t result, ColumnPtr & indexes)
{
    bool has_with_dictionary = false;
    bool convert_all_to_full = false;
    size_t column_with_dict_size = 0;

    for (auto & arg : args)
    {
        const auto & column = block.getByPosition(arg).column;
        if (auto * column_with_dict = checkAndGetColumn<ColumnWithDictionary>(column.get()))
        {
            if (has_with_dictionary)
                convert_all_to_full = true;
            else
            {
                has_with_dictionary = true;
                column_with_dict_size = column_with_dict->getUnique()->size();
                indexes = column_with_dict->getIndexesPtr();
            }
        }
        else if (!checkColumn<ColumnConst>(column.get()))
            convert_all_to_full = true;
    }

    if (!has_with_dictionary || convert_all_to_full)
        indexes = nullptr;

    if (!has_with_dictionary)
        return {};

    Block temp_block;
    temp_block.insert(block.getByPosition(result));
    {
        auto & column = temp_block.getByPosition(0);
        auto * type_with_dict = checkAndGetDataType<DataTypeWithDictionary>(column.type.get());
        if (!type_with_dict)
            throw Exception("Return type of function which has argument WithDictionary must be WithDictionary, got"
                            + column.type->getName(), ErrorCodes::LOGICAL_ERROR);

        column.type = type_with_dict->getDictionaryType();
    }

    for (auto & arg : args)
    {
        auto & column = block.getByPosition(arg);
        if (auto * column_with_dict = checkAndGetColumn<ColumnWithDictionary>(column.column.get()))
        {
            auto * type_with_dict = checkAndGetDataType<DataTypeWithDictionary>(column.type.get());
            if (!type_with_dict)
                throw Exception("Column with dictionary must have type WithDictionary, but has"
                                + column.type->getName(), ErrorCodes::LOGICAL_ERROR);

            ColumnPtr new_column = convert_all_to_full ? column_with_dict->convertToFullColumn()
                                                       : column_with_dict->getUnique()->getNestedColumn();

            temp_block.insert({new_column, type_with_dict->getDictionaryType(), column.name});
        }
        else if (auto * column_const = checkAndGetColumn<ColumnConst>(column.column.get()))
            temp_block.insert({column_const->cloneResized(column_with_dict_size), column.type, column.name});
        else if (convert_all_to_full)
            temp_block.insert(column);
        else
            throw Exception("Expected ColumnWithDictionary or ColumnConst, got" + column.column->getName(),
                            ErrorCodes::LOGICAL_ERROR);
    }

    return temp_block;
}

void PreparedFunctionImpl::execute(Block & block, const ColumnNumbers & args, size_t result, size_t input_rows_count)
{
    if (useDefaultImplementationForColumnsWithDictionary())
    {
        ColumnPtr indexes;
        Block temp_block = removeColumnsWithDictionary(block, args, result, indexes);
        if (temp_block)
        {
            ColumnNumbers temp_numbers(args.size());
            for (size_t i = 0; i < args.size(); ++i)
                temp_numbers[i] = i + 1;

            executeWithoutColumnsWithDictionary(temp_block, temp_numbers, 0, input_rows_count);
            auto & temp_res_col = temp_block.getByPosition(0).column;
            auto & res_col = block.getByPosition(result);
            auto col_wit_dict_ptr = res_col.type->createColumn();

            auto * col_with_dict = typeid_cast<ColumnWithDictionary *>(col_wit_dict_ptr.get());
            if (!col_with_dict)
                throw Exception("Expected ColumnWithDictionary, got" + res_col.column->getName(),
                                ErrorCodes::LOGICAL_ERROR);

            col_with_dict->insertRangeFromFullColumn(*temp_res_col, 0, temp_res_col->size());
            res_col.column = indexes ? col_with_dict->index(*indexes, 0)
                                     : std::move(col_wit_dict_ptr);
            return;
        }
    }

    executeWithoutColumnsWithDictionary(block, args, result, input_rows_count);
}

void FunctionBuilderImpl::checkNumberOfArguments(size_t number_of_arguments) const
{
    if (isVariadic())
        return;

    size_t expected_number_of_arguments = getNumberOfArguments();

    if (number_of_arguments != expected_number_of_arguments)
        throw Exception("Number of arguments for function " + getName() + " doesn't match: passed "
                        + toString(number_of_arguments) + ", should be " + toString(expected_number_of_arguments),
                        ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);
}

struct ArgumentsWithoutDictionary
{
    ColumnsWithTypeAndName arguments;
    DataTypePtr common_index_type;
    bool all_without_dictionary = true;

    explicit ArgumentsWithoutDictionary(const ColumnsWithTypeAndName & args)
    {
        DataTypes index_types;
        for (size_t i = 0; i < args.size(); ++i)
        {
            const auto & arg = args[i];
            if (auto * arg_with_dict = typeid_cast<const DataTypeWithDictionary*>(arg.type.get()))
            {
                if (all_without_dictionary)
                {
                    all_without_dictionary = false;
                    arguments = args;
                }
                arguments[i].type = arg_with_dict->getDictionaryType();
                index_types.push_back(arg_with_dict->getIndexesType());
            }
        }

        if (!all_without_dictionary)
            common_index_type = getLeastSupertype(index_types);
    }
};

DataTypePtr FunctionBuilderImpl::getReturnTypeWithoutDictionary(const ColumnsWithTypeAndName & arguments) const
{
    checkNumberOfArguments(arguments.size());

    if (!arguments.empty() && useDefaultImplementationForNulls())
    {
        NullPresence null_presense = getNullPresense(arguments);

        if (null_presense.has_null_constant)
        {
            return makeNullable(std::make_shared<DataTypeNothing>());
        }
        if (null_presense.has_nullable)
        {
            Block nested_block = createBlockWithNestedColumns(Block(arguments), ext::collection_cast<ColumnNumbers>(ext::range(0, arguments.size())));
            auto return_type = getReturnTypeImpl(ColumnsWithTypeAndName(nested_block.begin(), nested_block.end()));
            return makeNullable(return_type);

        }
    }

    return getReturnTypeImpl(arguments);
}

#if USE_EMBEDDED_COMPILER

static std::optional<DataTypes> removeNullables(const DataTypes & types)
{
    for (const auto & type : types)
    {
        if (!typeid_cast<const DataTypeNullable *>(type.get()))
            continue;
        DataTypes filtered;
        for (const auto & type : types)
            filtered.emplace_back(removeNullable(type));
        return filtered;
    }
    return {};
}

bool IFunction::isCompilable(const DataTypes & arguments) const
{
    if (useDefaultImplementationForNulls())
        if (auto denulled = removeNullables(arguments))
            return isCompilableImpl(*denulled);
    return isCompilableImpl(arguments);
}

llvm::Value * IFunction::compile(llvm::IRBuilderBase & builder, const DataTypes & arguments, ValuePlaceholders values) const
{
    if (useDefaultImplementationForNulls())
    {
        if (auto denulled = removeNullables(arguments))
        {
            /// FIXME: when only one column is nullable, this can actually be slower than the non-jitted version
            ///        because this involves copying the null map while `wrapInNullable` reuses it.
            auto & b = static_cast<llvm::IRBuilder<> &>(builder);
            auto * fail = llvm::BasicBlock::Create(b.GetInsertBlock()->getContext(), "", b.GetInsertBlock()->getParent());
            auto * join = llvm::BasicBlock::Create(b.GetInsertBlock()->getContext(), "", b.GetInsertBlock()->getParent());
            auto * zero = llvm::Constant::getNullValue(toNativeType(b, makeNullable(getReturnTypeImpl(*denulled))));
            for (size_t i = 0; i < arguments.size(); i++)
            {
                if (!arguments[i]->isNullable())
                    continue;
                /// Would be nice to evaluate all this lazily, but that'd change semantics: if only unevaluated
                /// arguments happen to contain NULLs, the return value would not be NULL, though it should be.
                auto * value = values[i]();
                auto * ok = llvm::BasicBlock::Create(b.GetInsertBlock()->getContext(), "", b.GetInsertBlock()->getParent());
                b.CreateCondBr(b.CreateExtractValue(value, {1}), fail, ok);
                b.SetInsertPoint(ok);
                values[i] = [value = b.CreateExtractValue(value, {0})]() { return value; };
            }
            auto * result = b.CreateInsertValue(zero, compileImpl(builder, *denulled, std::move(values)), {0});
            auto * result_block = b.GetInsertBlock();
            b.CreateBr(join);
            b.SetInsertPoint(fail);
            auto * null = b.CreateInsertValue(zero, b.getTrue(), {1});
            b.CreateBr(join);
            b.SetInsertPoint(join);
            auto * phi = b.CreatePHI(result->getType(), 2);
            phi->addIncoming(result, result_block);
            phi->addIncoming(null, fail);
            return phi;
        }
    }
    return compileImpl(builder, arguments, std::move(values));
}

#endif


DataTypePtr FunctionBuilderImpl::getReturnType(const ColumnsWithTypeAndName & arguments) const
{
    if (useDefaultImplementationForColumnsWithDictionary())
    {
        ArgumentsWithoutDictionary arguments_without_dictionary(arguments);
        if (!arguments_without_dictionary.all_without_dictionary)
            return std::make_shared<DataTypeWithDictionary>(
                    getReturnTypeWithoutDictionary(arguments_without_dictionary.arguments),
                    arguments_without_dictionary.common_index_type);
    }

    return getReturnTypeWithoutDictionary(arguments);
}
}
