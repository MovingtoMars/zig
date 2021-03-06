/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of zig, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "codegen.hpp"
#include "hash_map.hpp"
#include "zig_llvm.hpp"
#include "os.hpp"
#include "config.h"
#include "error.hpp"
#include "analyze.hpp"
#include "errmsg.hpp"
#include "ast_render.hpp"

#include <stdio.h>
#include <errno.h>

CodeGen *codegen_create(Buf *root_source_dir) {
    CodeGen *g = allocate<CodeGen>(1);
    g->link_table.init(32);
    g->import_table.init(32);
    g->builtin_fn_table.init(32);
    g->primitive_type_table.init(32);
    g->unresolved_top_level_decls.init(32);
    g->build_type = CodeGenBuildTypeDebug;
    g->root_source_dir = root_source_dir;
    g->next_error_index = 1;
    g->error_value_count = 1;

    return g;
}

void codegen_set_clang_argv(CodeGen *g, const char **args, int len) {
    g->clang_argv = args;
    g->clang_argv_len = len;
}

void codegen_set_build_type(CodeGen *g, CodeGenBuildType build_type) {
    g->build_type = build_type;
}

void codegen_set_is_static(CodeGen *g, bool is_static) {
    g->is_static = is_static;
}

void codegen_set_verbose(CodeGen *g, bool verbose) {
    g->verbose = verbose;
}

void codegen_set_errmsg_color(CodeGen *g, ErrColor err_color) {
    g->err_color = err_color;
}

void codegen_set_strip(CodeGen *g, bool strip) {
    g->strip_debug_symbols = strip;
}

void codegen_set_out_type(CodeGen *g, OutType out_type) {
    g->out_type = out_type;
}

void codegen_set_out_name(CodeGen *g, Buf *out_name) {
    g->root_out_name = out_name;
}

void codegen_set_libc_path(CodeGen *g, Buf *libc_path) {
    g->libc_path = libc_path;
}

static LLVMValueRef gen_expr(CodeGen *g, AstNode *expr_node);
static LLVMValueRef gen_lvalue(CodeGen *g, AstNode *expr_node, AstNode *node, TypeTableEntry **out_type_entry);
static LLVMValueRef gen_field_access_expr(CodeGen *g, AstNode *node, bool is_lvalue);
static LLVMValueRef gen_var_decl_raw(CodeGen *g, AstNode *source_node, AstNodeVariableDeclaration *var_decl,
        bool unwrap_maybe, LLVMValueRef *init_val);
static LLVMValueRef gen_assign_raw(CodeGen *g, AstNode *source_node, BinOpType bin_op,
        LLVMValueRef target_ref, LLVMValueRef value,
        TypeTableEntry *op1_type, TypeTableEntry *op2_type);

static TypeTableEntry *get_type_for_type_node(AstNode *node) {
    Expr *expr = get_resolved_expr(node);
    assert(expr->type_entry->id == TypeTableEntryIdMetaType);
    ConstExprValue *const_val = &expr->const_val;
    assert(const_val->ok);
    return const_val->data.x_type;
}

static void add_debug_source_node(CodeGen *g, AstNode *node) {
    assert(node->block_context);
    LLVMZigSetCurrentDebugLocation(g->builder, node->line + 1, node->column + 1, node->block_context->di_scope);
}

static TypeTableEntry *get_expr_type(AstNode *node) {
    return get_resolved_expr(node)->type_entry;
}

static TypeTableEntry *fn_proto_type_from_type_node(CodeGen *g, AstNode *type_node) {
    TypeTableEntry *type_entry = get_type_for_type_node(type_node);

    if (handle_is_ptr(type_entry)) {
        return get_pointer_to_type(g, type_entry, true);
    } else {
        return type_entry;
    }
}

enum AddSubMul {
    AddSubMulAdd = 0,
    AddSubMulSub = 1,
    AddSubMulMul = 2,
};

static int bits_index(int size_in_bits) {
    switch (size_in_bits) {
        case 8:
            return 0;
        case 16:
            return 1;
        case 32:
            return 2;
        case 64:
            return 3;
        default:
            zig_unreachable();
    }
}

static LLVMValueRef get_arithmetic_overflow_fn(CodeGen *g, TypeTableEntry *type_entry,
        const char *signed_name, const char *unsigned_name)
{
    const char *signed_str = type_entry->data.integral.is_signed ? signed_name : unsigned_name;
    Buf *llvm_name = buf_sprintf("llvm.%s.with.overflow.i%" PRIu64, signed_str, type_entry->size_in_bits);

    LLVMTypeRef return_elem_types[] = {
        type_entry->type_ref,
        LLVMInt1Type(),
    };
    LLVMTypeRef param_types[] = {
        type_entry->type_ref,
        type_entry->type_ref,
    };
    LLVMTypeRef return_struct_type = LLVMStructType(return_elem_types, 2, false);
    LLVMTypeRef fn_type = LLVMFunctionType(return_struct_type, param_types, 2, false);
    LLVMValueRef fn_val = LLVMAddFunction(g->module, buf_ptr(llvm_name), fn_type);
    assert(LLVMGetIntrinsicID(fn_val));
    return fn_val;
}

static LLVMValueRef get_int_overflow_fn(CodeGen *g, TypeTableEntry *type_entry, AddSubMul add_sub_mul) {
    assert(type_entry->id == TypeTableEntryIdInt);
    // [0-signed,1-unsigned][0-add,1-sub,2-mul][0-8,1-16,2-32,3-64]
    int index0 = type_entry->data.integral.is_signed ? 0 : 1;
    int index1 = add_sub_mul;
    int index2 = bits_index(type_entry->size_in_bits);
    LLVMValueRef *fn = &g->int_overflow_fns[index0][index1][index2];
    if (*fn) {
        return *fn;
    }
    switch (add_sub_mul) {
        case AddSubMulAdd:
            *fn = get_arithmetic_overflow_fn(g, type_entry, "sadd", "uadd");
            break;
        case AddSubMulSub:
            *fn = get_arithmetic_overflow_fn(g, type_entry, "ssub", "usub");
            break;
        case AddSubMulMul:
            *fn = get_arithmetic_overflow_fn(g, type_entry, "smul", "umul");
            break;

    }
    return *fn;
}


static LLVMValueRef gen_builtin_fn_call_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeFnCallExpr);
    AstNode *fn_ref_expr = node->data.fn_call_expr.fn_ref_expr;
    assert(fn_ref_expr->type == NodeTypeSymbol);
    BuiltinFnEntry *builtin_fn = node->data.fn_call_expr.builtin_fn;

    switch (builtin_fn->id) {
        case BuiltinFnIdInvalid:
        case BuiltinFnIdTypeof:
        case BuiltinFnIdCInclude:
        case BuiltinFnIdCDefine:
        case BuiltinFnIdCUndef:
            zig_unreachable();
        case BuiltinFnIdAddWithOverflow:
        case BuiltinFnIdSubWithOverflow:
        case BuiltinFnIdMulWithOverflow:
            {
                int fn_call_param_count = node->data.fn_call_expr.params.length;
                assert(fn_call_param_count == 4);

                TypeTableEntry *int_type = get_type_for_type_node(node->data.fn_call_expr.params.at(0));
                AddSubMul add_sub_mul;
                if (builtin_fn->id == BuiltinFnIdAddWithOverflow) {
                    add_sub_mul = AddSubMulAdd;
                } else if (builtin_fn->id == BuiltinFnIdSubWithOverflow) {
                    add_sub_mul = AddSubMulSub;
                } else if (builtin_fn->id == BuiltinFnIdMulWithOverflow) {
                    add_sub_mul = AddSubMulMul;
                } else {
                    zig_unreachable();
                }
                LLVMValueRef fn_val = get_int_overflow_fn(g, int_type, add_sub_mul);

                LLVMValueRef op1 = gen_expr(g, node->data.fn_call_expr.params.at(1));
                LLVMValueRef op2 = gen_expr(g, node->data.fn_call_expr.params.at(2));
                LLVMValueRef ptr_result = gen_expr(g, node->data.fn_call_expr.params.at(3));

                LLVMValueRef params[] = {
                    op1,
                    op2,
                };

                add_debug_source_node(g, node);
                LLVMValueRef result_struct = LLVMBuildCall(g->builder, fn_val, params, 2, "");
                LLVMValueRef result = LLVMBuildExtractValue(g->builder, result_struct, 0, "");
                LLVMValueRef overflow_bit = LLVMBuildExtractValue(g->builder, result_struct, 1, "");
                LLVMBuildStore(g->builder, result, ptr_result);

                return overflow_bit;
            }
        case BuiltinFnIdMemcpy:
            {
                int fn_call_param_count = node->data.fn_call_expr.params.length;
                assert(fn_call_param_count == 3);

                AstNode *dest_node = node->data.fn_call_expr.params.at(0);
                TypeTableEntry *dest_type = get_expr_type(dest_node);

                LLVMValueRef dest_ptr = gen_expr(g, dest_node);
                LLVMValueRef src_ptr = gen_expr(g, node->data.fn_call_expr.params.at(1));
                LLVMValueRef len_val = gen_expr(g, node->data.fn_call_expr.params.at(2));

                LLVMTypeRef ptr_u8 = LLVMPointerType(LLVMInt8Type(), 0);

                add_debug_source_node(g, node);
                LLVMValueRef dest_ptr_casted = LLVMBuildBitCast(g->builder, dest_ptr, ptr_u8, "");
                LLVMValueRef src_ptr_casted = LLVMBuildBitCast(g->builder, src_ptr, ptr_u8, "");

                uint64_t align_in_bytes = dest_type->data.pointer.child_type->align_in_bits / 8;

                LLVMValueRef params[] = {
                    dest_ptr_casted, // dest pointer
                    src_ptr_casted, // source pointer
                    len_val, // byte count
                    LLVMConstInt(LLVMInt32Type(), align_in_bytes, false), // align in bytes
                    LLVMConstNull(LLVMInt1Type()), // is volatile
                };

                LLVMBuildCall(g->builder, builtin_fn->fn_val, params, 5, "");
                return nullptr;
            }
        case BuiltinFnIdMemset:
            {
                int fn_call_param_count = node->data.fn_call_expr.params.length;
                assert(fn_call_param_count == 3);

                AstNode *dest_node = node->data.fn_call_expr.params.at(0);
                TypeTableEntry *dest_type = get_expr_type(dest_node);

                LLVMValueRef dest_ptr = gen_expr(g, dest_node);
                LLVMValueRef char_val = gen_expr(g, node->data.fn_call_expr.params.at(1));
                LLVMValueRef len_val = gen_expr(g, node->data.fn_call_expr.params.at(2));

                LLVMTypeRef ptr_u8 = LLVMPointerType(LLVMInt8Type(), 0);

                add_debug_source_node(g, node);
                LLVMValueRef dest_ptr_casted = LLVMBuildBitCast(g->builder, dest_ptr, ptr_u8, "");

                uint64_t align_in_bytes = dest_type->data.pointer.child_type->align_in_bits / 8;

                LLVMValueRef params[] = {
                    dest_ptr_casted, // dest pointer
                    char_val, // source pointer
                    len_val, // byte count
                    LLVMConstInt(LLVMInt32Type(), align_in_bytes, false), // align in bytes
                    LLVMConstNull(LLVMInt1Type()), // is volatile
                };

                LLVMBuildCall(g->builder, builtin_fn->fn_val, params, 5, "");
                return nullptr;
            }
        case BuiltinFnIdSizeof:
        case BuiltinFnIdMinValue:
        case BuiltinFnIdMaxValue:
        case BuiltinFnIdMemberCount:
            // caught by constant expression eval codegen
            zig_unreachable();
    }
    zig_unreachable();
}

static LLVMValueRef gen_enum_value_expr(CodeGen *g, AstNode *node, TypeTableEntry *enum_type,
        AstNode *arg_node)
{
    assert(node->type == NodeTypeFieldAccessExpr);

    uint64_t value = node->data.field_access_expr.type_enum_field->value;
    LLVMTypeRef tag_type_ref = enum_type->data.enumeration.tag_type->type_ref;
    LLVMValueRef tag_value = LLVMConstInt(tag_type_ref, value, false);

    if (enum_type->data.enumeration.gen_field_count == 0) {
        return tag_value;
    } else {
        TypeTableEntry *arg_node_type = nullptr;
        LLVMValueRef new_union_val = gen_expr(g, arg_node);
        if (arg_node) {
            arg_node_type = get_expr_type(arg_node);
            new_union_val = gen_expr(g, arg_node);
        } else {
            arg_node_type = g->builtin_types.entry_void;
        }

        LLVMValueRef tmp_struct_ptr = node->data.field_access_expr.resolved_struct_val_expr.ptr;

        // populate the new tag value
        add_debug_source_node(g, node);
        LLVMValueRef tag_field_ptr = LLVMBuildStructGEP(g->builder, tmp_struct_ptr, 0, "");
        LLVMBuildStore(g->builder, tag_value, tag_field_ptr);

        if (arg_node_type->id != TypeTableEntryIdVoid) {
            // populate the union value
            TypeTableEntry *union_val_type = get_expr_type(arg_node);
            LLVMValueRef union_field_ptr = LLVMBuildStructGEP(g->builder, tmp_struct_ptr, 1, "");
            LLVMValueRef bitcasted_union_field_ptr = LLVMBuildBitCast(g->builder, union_field_ptr,
                    LLVMPointerType(union_val_type->type_ref, 0), "");

            gen_assign_raw(g, arg_node, BinOpTypeAssign, bitcasted_union_field_ptr, new_union_val,
                    union_val_type, union_val_type);

        }

        return tmp_struct_ptr;
    }
}

static LLVMValueRef gen_widen_or_shorten(CodeGen *g, AstNode *source_node, TypeTableEntry *actual_type,
        TypeTableEntry *wanted_type, LLVMValueRef expr_val)
{
    if (actual_type->size_in_bits == wanted_type->size_in_bits) {
        return expr_val;
    } else if (actual_type->size_in_bits < wanted_type->size_in_bits) {
        if (actual_type->data.integral.is_signed) {
            add_debug_source_node(g, source_node);
            return LLVMBuildSExt(g->builder, expr_val, wanted_type->type_ref, "");
        } else {
            add_debug_source_node(g, source_node);
            return LLVMBuildZExt(g->builder, expr_val, wanted_type->type_ref, "");
        }
    } else {
        assert(actual_type->size_in_bits > wanted_type->size_in_bits);
        add_debug_source_node(g, source_node);
        return LLVMBuildTrunc(g->builder, expr_val, wanted_type->type_ref, "");
    }
}

static LLVMValueRef gen_cast_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeFnCallExpr);

    AstNode *expr_node = node->data.fn_call_expr.params.at(0);

    LLVMValueRef expr_val = gen_expr(g, expr_node);

    TypeTableEntry *actual_type = get_expr_type(expr_node);
    TypeTableEntry *wanted_type = get_expr_type(node);

    AstNodeFnCallExpr *cast_expr = &node->data.fn_call_expr;

    switch (cast_expr->cast_op) {
        case CastOpNoCast:
            zig_unreachable();
        case CastOpNoop:
            return expr_val;
        case CastOpErrToInt:
            assert(actual_type->id == TypeTableEntryIdErrorUnion);
            if (actual_type->data.error.child_type->size_in_bits == 0) {
                return gen_widen_or_shorten(g, node, g->err_tag_type, wanted_type, expr_val);
            } else {
                zig_panic("TODO");
            }
        case CastOpMaybeWrap:
            {
                assert(cast_expr->tmp_ptr);
                assert(wanted_type->id == TypeTableEntryIdMaybe);
                assert(actual_type);

                add_debug_source_node(g, node);
                LLVMValueRef val_ptr = LLVMBuildStructGEP(g->builder, cast_expr->tmp_ptr, 0, "");
                gen_assign_raw(g, node, BinOpTypeAssign,
                        val_ptr, expr_val, wanted_type->data.maybe.child_type, actual_type);

                add_debug_source_node(g, node);
                LLVMValueRef maybe_ptr = LLVMBuildStructGEP(g->builder, cast_expr->tmp_ptr, 1, "");
                LLVMBuildStore(g->builder, LLVMConstAllOnes(LLVMInt1Type()), maybe_ptr);

                return cast_expr->tmp_ptr;
            }
        case CastOpErrorWrap:
            {
                assert(wanted_type->id == TypeTableEntryIdErrorUnion);
                TypeTableEntry *child_type = wanted_type->data.error.child_type;
                LLVMValueRef ok_err_val = LLVMConstNull(g->err_tag_type->type_ref);

                if (child_type->size_in_bits == 0) {
                    return ok_err_val;
                } else {
                    assert(cast_expr->tmp_ptr);
                    assert(wanted_type->id == TypeTableEntryIdErrorUnion);
                    assert(actual_type);

                    add_debug_source_node(g, node);
                    LLVMValueRef err_tag_ptr = LLVMBuildStructGEP(g->builder, cast_expr->tmp_ptr, 0, "");
                    LLVMBuildStore(g->builder, ok_err_val, err_tag_ptr);

                    LLVMValueRef payload_ptr = LLVMBuildStructGEP(g->builder, cast_expr->tmp_ptr, 1, "");
                    gen_assign_raw(g, node, BinOpTypeAssign,
                            payload_ptr, expr_val, child_type, actual_type);

                    return cast_expr->tmp_ptr;
                }
            }
        case CastOpPureErrorWrap:
            assert(wanted_type->id == TypeTableEntryIdErrorUnion);
            if (wanted_type->data.error.child_type->size_in_bits == 0) {
                return expr_val;
            } else {
                zig_panic("TODO");
            }
        case CastOpPtrToInt:
            add_debug_source_node(g, node);
            return LLVMBuildPtrToInt(g->builder, expr_val, wanted_type->type_ref, "");
        case CastOpIntToPtr:
            add_debug_source_node(g, node);
            return LLVMBuildIntToPtr(g->builder, expr_val, wanted_type->type_ref, "");
        case CastOpPointerReinterpret:
            add_debug_source_node(g, node);
            return LLVMBuildBitCast(g->builder, expr_val, wanted_type->type_ref, "");
        case CastOpIntWidenOrShorten:
            return gen_widen_or_shorten(g, node, actual_type, wanted_type, expr_val);
        case CastOpToUnknownSizeArray:
            {
                assert(cast_expr->tmp_ptr);
                assert(wanted_type->id == TypeTableEntryIdStruct);
                assert(wanted_type->data.structure.is_unknown_size_array);

                TypeTableEntry *pointer_type = wanted_type->data.structure.fields[0].type_entry;

                add_debug_source_node(g, node);

                LLVMValueRef ptr_ptr = LLVMBuildStructGEP(g->builder, cast_expr->tmp_ptr, 0, "");
                LLVMValueRef expr_bitcast = LLVMBuildBitCast(g->builder, expr_val, pointer_type->type_ref, "");
                LLVMBuildStore(g->builder, expr_bitcast, ptr_ptr);

                LLVMValueRef len_ptr = LLVMBuildStructGEP(g->builder, cast_expr->tmp_ptr, 1, "");
                LLVMValueRef len_val = LLVMConstInt(g->builtin_types.entry_isize->type_ref,
                        actual_type->data.array.len, false);
                LLVMBuildStore(g->builder, len_val, len_ptr);

                return cast_expr->tmp_ptr;
            }
    }
    zig_unreachable();
}


static LLVMValueRef gen_fn_call_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeFnCallExpr);

    if (node->data.fn_call_expr.is_builtin) {
        return gen_builtin_fn_call_expr(g, node);
    } else if (node->data.fn_call_expr.cast_op != CastOpNoCast) {
        return gen_cast_expr(g, node);
    }

    FnTableEntry *fn_table_entry = node->data.fn_call_expr.fn_entry;
    AstNode *fn_ref_expr = node->data.fn_call_expr.fn_ref_expr;
    TypeTableEntry *struct_type = nullptr;
    AstNode *first_param_expr = nullptr;
    if (fn_ref_expr->type == NodeTypeFieldAccessExpr) {
        first_param_expr = fn_ref_expr->data.field_access_expr.struct_expr;
        struct_type = get_expr_type(first_param_expr);
        if (struct_type->id == TypeTableEntryIdStruct) {
            fn_table_entry = node->data.fn_call_expr.fn_entry;
        } else if (struct_type->id == TypeTableEntryIdPointer) {
            assert(struct_type->data.pointer.child_type->id == TypeTableEntryIdStruct);
            fn_table_entry = node->data.fn_call_expr.fn_entry;
        } else if (struct_type->id == TypeTableEntryIdMetaType) {
            TypeTableEntry *enum_type = get_type_for_type_node(first_param_expr);
            int param_count = node->data.fn_call_expr.params.length;
            AstNode *arg1_node;
            if (param_count == 1) {
                arg1_node = node->data.fn_call_expr.params.at(0);
            } else {
                assert(param_count == 0);
                arg1_node = nullptr;
            }
            return gen_enum_value_expr(g, fn_ref_expr, enum_type, arg1_node);
        } else {
            zig_unreachable();
        }
    }

    TypeTableEntry *fn_type;
    LLVMValueRef fn_val;
    if (fn_table_entry) {
        fn_val = fn_table_entry->fn_value;
        fn_type = fn_table_entry->type_entry;
    } else {
        fn_val = gen_expr(g, fn_ref_expr);
        fn_type = get_expr_type(fn_ref_expr);
    }

    TypeTableEntry *src_return_type = fn_type->data.fn.src_return_type;

    int fn_call_param_count = node->data.fn_call_expr.params.length;
    bool first_arg_ret = handle_is_ptr(src_return_type);
    int actual_param_count = fn_call_param_count + (struct_type ? 1 : 0) + (first_arg_ret ? 1 : 0);
    bool is_var_args = fn_type->data.fn.is_var_args;

    // don't really include void values
    LLVMValueRef *gen_param_values = allocate<LLVMValueRef>(actual_param_count);

    int gen_param_index = 0;
    if (first_arg_ret) {
        gen_param_values[gen_param_index] = node->data.fn_call_expr.tmp_ptr;
        gen_param_index += 1;
    }
    if (struct_type) {
        gen_param_values[gen_param_index] = gen_expr(g, first_param_expr);
        gen_param_index += 1;
    }

    for (int i = 0; i < fn_call_param_count; i += 1) {
        AstNode *expr_node = node->data.fn_call_expr.params.at(i);
        LLVMValueRef param_value = gen_expr(g, expr_node);
        TypeTableEntry *param_type = get_expr_type(expr_node);
        if (is_var_args || param_type->size_in_bits > 0) {
            gen_param_values[gen_param_index] = param_value;
            gen_param_index += 1;
        }
    }

    add_debug_source_node(g, node);
    LLVMValueRef result = LLVMZigBuildCall(g->builder, fn_val,
            gen_param_values, gen_param_index, fn_type->data.fn.calling_convention, "");

    if (src_return_type->id == TypeTableEntryIdUnreachable) {
        return LLVMBuildUnreachable(g->builder);
    } else if (first_arg_ret) {
        return node->data.fn_call_expr.tmp_ptr;
    } else {
        return result;
    }
}

static LLVMValueRef gen_array_base_ptr(CodeGen *g, AstNode *node) {
    TypeTableEntry *type_entry = get_expr_type(node);

    LLVMValueRef array_ptr;
    if (node->type == NodeTypeFieldAccessExpr) {
        array_ptr = gen_field_access_expr(g, node, true);
        if (type_entry->id == TypeTableEntryIdPointer) {
            // we have a double pointer so we must dereference it once
            add_debug_source_node(g, node);
            array_ptr = LLVMBuildLoad(g->builder, array_ptr, "");
        }
    } else {
        array_ptr = gen_expr(g, node);
    }

    assert(!array_ptr || LLVMGetTypeKind(LLVMTypeOf(array_ptr)) == LLVMPointerTypeKind);

    return array_ptr;
}

static LLVMValueRef gen_array_elem_ptr(CodeGen *g, AstNode *source_node, LLVMValueRef array_ptr,
        TypeTableEntry *array_type, LLVMValueRef subscript_value)
{
    assert(subscript_value);

    if (array_type->size_in_bits == 0) {
        return nullptr;
    }

    if (array_type->id == TypeTableEntryIdArray) {
        LLVMValueRef indices[] = {
            LLVMConstNull(g->builtin_types.entry_isize->type_ref),
            subscript_value
        };
        add_debug_source_node(g, source_node);
        return LLVMBuildInBoundsGEP(g->builder, array_ptr, indices, 2, "");
    } else if (array_type->id == TypeTableEntryIdPointer) {
        assert(LLVMGetTypeKind(LLVMTypeOf(array_ptr)) == LLVMPointerTypeKind);
        LLVMValueRef indices[] = {
            subscript_value
        };
        add_debug_source_node(g, source_node);
        return LLVMBuildInBoundsGEP(g->builder, array_ptr, indices, 1, "");
    } else if (array_type->id == TypeTableEntryIdStruct) {
        assert(array_type->data.structure.is_unknown_size_array);
        assert(LLVMGetTypeKind(LLVMTypeOf(array_ptr)) == LLVMPointerTypeKind);
        assert(LLVMGetTypeKind(LLVMGetElementType(LLVMTypeOf(array_ptr))) == LLVMStructTypeKind);

        add_debug_source_node(g, source_node);
        LLVMValueRef ptr_ptr = LLVMBuildStructGEP(g->builder, array_ptr, 0, "");
        LLVMValueRef ptr = LLVMBuildLoad(g->builder, ptr_ptr, "");
        return LLVMBuildInBoundsGEP(g->builder, ptr, &subscript_value, 1, "");
    } else {
        zig_unreachable();
    }
}

static LLVMValueRef gen_array_ptr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeArrayAccessExpr);

    AstNode *array_expr_node = node->data.array_access_expr.array_ref_expr;
    TypeTableEntry *array_type = get_expr_type(array_expr_node);

    LLVMValueRef array_ptr = gen_array_base_ptr(g, array_expr_node);

    LLVMValueRef subscript_value = gen_expr(g, node->data.array_access_expr.subscript);

    return gen_array_elem_ptr(g, node, array_ptr, array_type, subscript_value);
}

static LLVMValueRef gen_field_ptr(CodeGen *g, AstNode *node, TypeTableEntry **out_type_entry) {
    assert(node->type == NodeTypeFieldAccessExpr);

    AstNode *struct_expr_node = node->data.field_access_expr.struct_expr;

    LLVMValueRef struct_ptr;
    if (struct_expr_node->type == NodeTypeSymbol) {
        VariableTableEntry *var = find_variable(struct_expr_node->block_context,
                &struct_expr_node->data.symbol_expr.symbol);
        assert(var);

        if (var->is_ptr && var->type->id == TypeTableEntryIdPointer) {
            add_debug_source_node(g, node);
            struct_ptr = LLVMBuildLoad(g->builder, var->value_ref, "");
        } else {
            struct_ptr = var->value_ref;
        }
    } else if (struct_expr_node->type == NodeTypeFieldAccessExpr) {
        struct_ptr = gen_field_access_expr(g, struct_expr_node, true);
        TypeTableEntry *field_type = get_expr_type(struct_expr_node);
        if (field_type->id == TypeTableEntryIdPointer) {
            // we have a double pointer so we must dereference it once
            add_debug_source_node(g, node);
            struct_ptr = LLVMBuildLoad(g->builder, struct_ptr, "");
        }
    } else {
        struct_ptr = gen_expr(g, struct_expr_node);
    }

    assert(LLVMGetTypeKind(LLVMTypeOf(struct_ptr)) == LLVMPointerTypeKind);
    assert(LLVMGetTypeKind(LLVMGetElementType(LLVMTypeOf(struct_ptr))) == LLVMStructTypeKind);

    int gen_field_index = node->data.field_access_expr.type_struct_field->gen_index;
    assert(gen_field_index >= 0);

    *out_type_entry = node->data.field_access_expr.type_struct_field->type_entry;

    add_debug_source_node(g, node);
    return LLVMBuildStructGEP(g->builder, struct_ptr, gen_field_index, "");
}

static LLVMValueRef gen_slice_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeSliceExpr);

    AstNode *array_ref_node = node->data.slice_expr.array_ref_expr;
    TypeTableEntry *array_type = get_expr_type(array_ref_node);

    LLVMValueRef tmp_struct_ptr = node->data.slice_expr.resolved_struct_val_expr.ptr;
    LLVMValueRef array_ptr = gen_array_base_ptr(g, array_ref_node);

    if (array_type->id == TypeTableEntryIdArray) {
        LLVMValueRef start_val = gen_expr(g, node->data.slice_expr.start);
        LLVMValueRef end_val;
        if (node->data.slice_expr.end) {
            end_val = gen_expr(g, node->data.slice_expr.end);
        } else {
            end_val = LLVMConstInt(g->builtin_types.entry_isize->type_ref, array_type->data.array.len, false);
        }

        add_debug_source_node(g, node);
        LLVMValueRef ptr_field_ptr = LLVMBuildStructGEP(g->builder, tmp_struct_ptr, 0, "");
        LLVMValueRef indices[] = {
            LLVMConstNull(g->builtin_types.entry_isize->type_ref),
            start_val,
        };
        LLVMValueRef slice_start_ptr = LLVMBuildInBoundsGEP(g->builder, array_ptr, indices, 2, "");
        LLVMBuildStore(g->builder, slice_start_ptr, ptr_field_ptr);

        LLVMValueRef len_field_ptr = LLVMBuildStructGEP(g->builder, tmp_struct_ptr, 1, "");
        LLVMValueRef len_value = LLVMBuildSub(g->builder, end_val, start_val, "");
        LLVMBuildStore(g->builder, len_value, len_field_ptr);

        return tmp_struct_ptr;
    } else if (array_type->id == TypeTableEntryIdPointer) {
        LLVMValueRef start_val = gen_expr(g, node->data.slice_expr.start);
        LLVMValueRef end_val = gen_expr(g, node->data.slice_expr.end);

        add_debug_source_node(g, node);
        LLVMValueRef ptr_field_ptr = LLVMBuildStructGEP(g->builder, tmp_struct_ptr, 0, "");
        LLVMValueRef slice_start_ptr = LLVMBuildInBoundsGEP(g->builder, array_ptr, &start_val, 1, "");
        LLVMBuildStore(g->builder, slice_start_ptr, ptr_field_ptr);

        LLVMValueRef len_field_ptr = LLVMBuildStructGEP(g->builder, tmp_struct_ptr, 1, "");
        LLVMValueRef len_value = LLVMBuildSub(g->builder, end_val, start_val, "");
        LLVMBuildStore(g->builder, len_value, len_field_ptr);

        return tmp_struct_ptr;
    } else if (array_type->id == TypeTableEntryIdStruct) {
        assert(array_type->data.structure.is_unknown_size_array);
        assert(LLVMGetTypeKind(LLVMTypeOf(array_ptr)) == LLVMPointerTypeKind);
        assert(LLVMGetTypeKind(LLVMGetElementType(LLVMTypeOf(array_ptr))) == LLVMStructTypeKind);

        LLVMValueRef start_val = gen_expr(g, node->data.slice_expr.start);
        LLVMValueRef end_val;
        if (node->data.slice_expr.end) {
            end_val = gen_expr(g, node->data.slice_expr.end);
        } else {
            add_debug_source_node(g, node);
            LLVMValueRef src_len_ptr = LLVMBuildStructGEP(g->builder, array_ptr, 1, "");
            end_val = LLVMBuildLoad(g->builder, src_len_ptr, "");
        }

        add_debug_source_node(g, node);
        LLVMValueRef src_ptr_ptr = LLVMBuildStructGEP(g->builder, array_ptr, 0, "");
        LLVMValueRef src_ptr = LLVMBuildLoad(g->builder, src_ptr_ptr, "");
        LLVMValueRef ptr_field_ptr = LLVMBuildStructGEP(g->builder, tmp_struct_ptr, 0, "");
        LLVMValueRef slice_start_ptr = LLVMBuildInBoundsGEP(g->builder, src_ptr, &start_val, 1, "");
        LLVMBuildStore(g->builder, slice_start_ptr, ptr_field_ptr);

        LLVMValueRef len_field_ptr = LLVMBuildStructGEP(g->builder, tmp_struct_ptr, 1, "");
        LLVMValueRef len_value = LLVMBuildSub(g->builder, end_val, start_val, "");
        LLVMBuildStore(g->builder, len_value, len_field_ptr);

        return tmp_struct_ptr;
    } else {
        zig_unreachable();
    }
}


static LLVMValueRef gen_array_access_expr(CodeGen *g, AstNode *node, bool is_lvalue) {
    assert(node->type == NodeTypeArrayAccessExpr);

    LLVMValueRef ptr = gen_array_ptr(g, node);
    TypeTableEntry *child_type;
    TypeTableEntry *array_type = get_expr_type(node->data.array_access_expr.array_ref_expr);
    if (array_type->id == TypeTableEntryIdPointer) {
        child_type = array_type->data.pointer.child_type;
    } else if (array_type->id == TypeTableEntryIdStruct) {
        assert(array_type->data.structure.is_unknown_size_array);
        TypeTableEntry *child_ptr_type = array_type->data.structure.fields[0].type_entry;
        assert(child_ptr_type->id == TypeTableEntryIdPointer);
        child_type = child_ptr_type->data.pointer.child_type;
    } else if (array_type->id == TypeTableEntryIdArray) {
        child_type = array_type->data.array.child_type;
    } else {
        zig_unreachable();
    }

    if (is_lvalue || !ptr || handle_is_ptr(child_type)) {
        return ptr;
    } else {
        add_debug_source_node(g, node);
        return LLVMBuildLoad(g->builder, ptr, "");
    }
}

static LLVMValueRef gen_field_access_expr(CodeGen *g, AstNode *node, bool is_lvalue) {
    assert(node->type == NodeTypeFieldAccessExpr);

    AstNode *struct_expr = node->data.field_access_expr.struct_expr;
    TypeTableEntry *struct_type = get_expr_type(struct_expr);
    Buf *name = &node->data.field_access_expr.field_name;

    if (struct_type->id == TypeTableEntryIdArray) {
        if (buf_eql_str(name, "len")) {
            return LLVMConstInt(g->builtin_types.entry_isize->type_ref,
                    struct_type->data.array.len, false);
        } else if (buf_eql_str(name, "ptr")) {
            LLVMValueRef array_val = gen_expr(g, node->data.field_access_expr.struct_expr);
            LLVMValueRef indices[] = {
                LLVMConstNull(g->builtin_types.entry_isize->type_ref),
                LLVMConstNull(g->builtin_types.entry_isize->type_ref),
            };
            add_debug_source_node(g, node);
            return LLVMBuildInBoundsGEP(g->builder, array_val, indices, 2, "");
        } else {
            zig_panic("gen_field_access_expr bad array field");
        }
    } else if (struct_type->id == TypeTableEntryIdStruct || (struct_type->id == TypeTableEntryIdPointer &&
               struct_type->data.pointer.child_type->id == TypeTableEntryIdStruct))
    {
        TypeTableEntry *type_entry;
        LLVMValueRef ptr = gen_field_ptr(g, node, &type_entry);
        if (is_lvalue || handle_is_ptr(type_entry)) {
            return ptr;
        } else {
            add_debug_source_node(g, node);
            return LLVMBuildLoad(g->builder, ptr, "");
        }
    } else if (struct_type->id == TypeTableEntryIdMetaType) {
        assert(!is_lvalue);
        TypeTableEntry *enum_type = get_type_for_type_node(struct_expr);
        return gen_enum_value_expr(g, node, enum_type, nullptr);
    } else {
        zig_unreachable();
    }
}

static LLVMValueRef gen_lvalue(CodeGen *g, AstNode *expr_node, AstNode *node,
        TypeTableEntry **out_type_entry)
{
    LLVMValueRef target_ref;

    if (node->type == NodeTypeSymbol) {
        VariableTableEntry *var = find_variable(expr_node->block_context,
                &node->data.symbol_expr.symbol);
        assert(var);

        *out_type_entry = var->type;
        target_ref = var->value_ref;
    } else if (node->type == NodeTypeArrayAccessExpr) {
        TypeTableEntry *array_type = get_expr_type(node->data.array_access_expr.array_ref_expr);
        if (array_type->id == TypeTableEntryIdArray) {
            *out_type_entry = array_type->data.array.child_type;
            target_ref = gen_array_ptr(g, node);
        } else if (array_type->id == TypeTableEntryIdPointer) {
            *out_type_entry = array_type->data.pointer.child_type;
            target_ref = gen_array_ptr(g, node);
        } else if (array_type->id == TypeTableEntryIdStruct) {
            assert(array_type->data.structure.is_unknown_size_array);
            *out_type_entry = array_type->data.structure.fields[0].type_entry->data.pointer.child_type;
            target_ref = gen_array_ptr(g, node);
        } else {
            zig_unreachable();
        }
    } else if (node->type == NodeTypeFieldAccessExpr) {
        target_ref = gen_field_ptr(g, node, out_type_entry);
    } else if (node->type == NodeTypePrefixOpExpr) {
        assert(node->data.prefix_op_expr.prefix_op == PrefixOpDereference);
        AstNode *target_expr = node->data.prefix_op_expr.primary_expr;
        TypeTableEntry *type_entry = get_expr_type(target_expr);
        assert(type_entry->id == TypeTableEntryIdPointer);
        *out_type_entry = type_entry->data.pointer.child_type;
        return gen_expr(g, target_expr);
    } else {
        zig_panic("bad assign target");
    }

    return target_ref;
}

static LLVMValueRef gen_prefix_op_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypePrefixOpExpr);
    assert(node->data.prefix_op_expr.primary_expr);

    AstNode *expr_node = node->data.prefix_op_expr.primary_expr;

    switch (node->data.prefix_op_expr.prefix_op) {
        case PrefixOpInvalid:
            zig_unreachable();
        case PrefixOpNegation:
            {
                LLVMValueRef expr = gen_expr(g, expr_node);
                add_debug_source_node(g, node);
                return LLVMBuildNeg(g->builder, expr, "");
            }
        case PrefixOpBoolNot:
            {
                LLVMValueRef expr = gen_expr(g, expr_node);
                LLVMValueRef zero = LLVMConstNull(LLVMTypeOf(expr));
                add_debug_source_node(g, node);
                return LLVMBuildICmp(g->builder, LLVMIntEQ, expr, zero, "");
            }
        case PrefixOpBinNot:
            {
                LLVMValueRef expr = gen_expr(g, expr_node);
                add_debug_source_node(g, node);
                return LLVMBuildNot(g->builder, expr, "");
            }
        case PrefixOpAddressOf:
        case PrefixOpConstAddressOf:
            {
                TypeTableEntry *lvalue_type;
                return gen_lvalue(g, node, expr_node, &lvalue_type);
            }

        case PrefixOpDereference:
            {
                LLVMValueRef expr = gen_expr(g, expr_node);
                TypeTableEntry *type_entry = get_expr_type(expr_node);
                if (type_entry->size_in_bits == 0) {
                    return nullptr;
                } else {
                    add_debug_source_node(g, node);
                    return LLVMBuildLoad(g->builder, expr, "");
                }
            }
        case PrefixOpMaybe:
            {
                zig_panic("TODO codegen PrefixOpMaybe");
            }
        case PrefixOpError:
            {
                zig_panic("TODO codegen PrefixOpError");
            }
        case PrefixOpUnwrapError:
            {
                LLVMValueRef expr_val = gen_expr(g, expr_node);
                TypeTableEntry *expr_type = get_expr_type(expr_node);
                assert(expr_type->id == TypeTableEntryIdErrorUnion);
                TypeTableEntry *child_type = expr_type->data.error.child_type;
                // TODO in debug mode, put a panic here if the error is not 0
                if (child_type->size_in_bits > 0) {
                    add_debug_source_node(g, node);
                    LLVMValueRef child_val_ptr = LLVMBuildStructGEP(g->builder, expr_val, 1, "");
                    if (handle_is_ptr(child_type)) {
                        return child_val_ptr;
                    } else {
                        return LLVMBuildLoad(g->builder, child_val_ptr, "");
                    }
                } else {
                    return nullptr;
                }
            }
    }
    zig_unreachable();
}

static LLVMValueRef gen_arithmetic_bin_op(CodeGen *g, AstNode *source_node,
    LLVMValueRef val1, LLVMValueRef val2,
    TypeTableEntry *op1_type, TypeTableEntry *op2_type,
    BinOpType bin_op)
{
    assert(op1_type == op2_type);

    switch (bin_op) {
        case BinOpTypeBinOr:
        case BinOpTypeAssignBitOr:
            add_debug_source_node(g, source_node);
            return LLVMBuildOr(g->builder, val1, val2, "");
        case BinOpTypeBinXor:
        case BinOpTypeAssignBitXor:
            add_debug_source_node(g, source_node);
            return LLVMBuildXor(g->builder, val1, val2, "");
        case BinOpTypeBinAnd:
        case BinOpTypeAssignBitAnd:
            add_debug_source_node(g, source_node);
            return LLVMBuildAnd(g->builder, val1, val2, "");
        case BinOpTypeBitShiftLeft:
        case BinOpTypeAssignBitShiftLeft:
            add_debug_source_node(g, source_node);
            return LLVMBuildShl(g->builder, val1, val2, "");
        case BinOpTypeBitShiftRight:
        case BinOpTypeAssignBitShiftRight:
            assert(op1_type->id == TypeTableEntryIdInt);
            assert(op2_type->id == TypeTableEntryIdInt);

            add_debug_source_node(g, source_node);
            if (op1_type->data.integral.is_signed) {
                return LLVMBuildAShr(g->builder, val1, val2, "");
            } else {
                return LLVMBuildLShr(g->builder, val1, val2, "");
            }
        case BinOpTypeAdd:
        case BinOpTypeAssignPlus:
            add_debug_source_node(g, source_node);
            if (op1_type->id == TypeTableEntryIdFloat) {
                return LLVMBuildFAdd(g->builder, val1, val2, "");
            } else {
                return LLVMBuildAdd(g->builder, val1, val2, "");
            }
        case BinOpTypeSub:
        case BinOpTypeAssignMinus:
            add_debug_source_node(g, source_node);
            if (op1_type->id == TypeTableEntryIdFloat) {
                return LLVMBuildFSub(g->builder, val1, val2, "");
            } else {
                return LLVMBuildSub(g->builder, val1, val2, "");
            }
        case BinOpTypeMult:
        case BinOpTypeAssignTimes:
            add_debug_source_node(g, source_node);
            if (op1_type->id == TypeTableEntryIdFloat) {
                return LLVMBuildFMul(g->builder, val1, val2, "");
            } else {
                return LLVMBuildMul(g->builder, val1, val2, "");
            }
        case BinOpTypeDiv:
        case BinOpTypeAssignDiv:
            add_debug_source_node(g, source_node);
            if (op1_type->id == TypeTableEntryIdFloat) {
                return LLVMBuildFDiv(g->builder, val1, val2, "");
            } else {
                assert(op1_type->id == TypeTableEntryIdInt);
                if (op1_type->data.integral.is_signed) {
                    return LLVMBuildSDiv(g->builder, val1, val2, "");
                } else {
                    return LLVMBuildUDiv(g->builder, val1, val2, "");
                }
            }
        case BinOpTypeMod:
        case BinOpTypeAssignMod:
            add_debug_source_node(g, source_node);
            if (op1_type->id == TypeTableEntryIdFloat) {
                return LLVMBuildFRem(g->builder, val1, val2, "");
            } else {
                assert(op1_type->id == TypeTableEntryIdInt);
                if (op1_type->data.integral.is_signed) {
                    return LLVMBuildSRem(g->builder, val1, val2, "");
                } else {
                    return LLVMBuildURem(g->builder, val1, val2, "");
                }
            }
        case BinOpTypeBoolOr:
        case BinOpTypeBoolAnd:
        case BinOpTypeCmpEq:
        case BinOpTypeCmpNotEq:
        case BinOpTypeCmpLessThan:
        case BinOpTypeCmpGreaterThan:
        case BinOpTypeCmpLessOrEq:
        case BinOpTypeCmpGreaterOrEq:
        case BinOpTypeInvalid:
        case BinOpTypeAssign:
        case BinOpTypeAssignBoolAnd:
        case BinOpTypeAssignBoolOr:
        case BinOpTypeUnwrapMaybe:
        case BinOpTypeStrCat:
            zig_unreachable();
    }
    zig_unreachable();
}
static LLVMValueRef gen_arithmetic_bin_op_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeBinOpExpr);

    LLVMValueRef val1 = gen_expr(g, node->data.bin_op_expr.op1);
    LLVMValueRef val2 = gen_expr(g, node->data.bin_op_expr.op2);

    TypeTableEntry *op1_type = get_expr_type(node->data.bin_op_expr.op1);
    TypeTableEntry *op2_type = get_expr_type(node->data.bin_op_expr.op2);
    return gen_arithmetic_bin_op(g, node, val1, val2, op1_type, op2_type, node->data.bin_op_expr.bin_op);

}

static LLVMIntPredicate cmp_op_to_int_predicate(BinOpType cmp_op, bool is_signed) {
    switch (cmp_op) {
        case BinOpTypeCmpEq:
            return LLVMIntEQ;
        case BinOpTypeCmpNotEq:
            return LLVMIntNE;
        case BinOpTypeCmpLessThan:
            return is_signed ? LLVMIntSLT : LLVMIntULT;
        case BinOpTypeCmpGreaterThan:
            return is_signed ? LLVMIntSGT : LLVMIntUGT;
        case BinOpTypeCmpLessOrEq:
            return is_signed ? LLVMIntSLE : LLVMIntULE;
        case BinOpTypeCmpGreaterOrEq:
            return is_signed ? LLVMIntSGE : LLVMIntUGE;
        default:
            zig_unreachable();
    }
}

static LLVMRealPredicate cmp_op_to_real_predicate(BinOpType cmp_op) {
    switch (cmp_op) {
        case BinOpTypeCmpEq:
            return LLVMRealOEQ;
        case BinOpTypeCmpNotEq:
            return LLVMRealONE;
        case BinOpTypeCmpLessThan:
            return LLVMRealOLT;
        case BinOpTypeCmpGreaterThan:
            return LLVMRealOGT;
        case BinOpTypeCmpLessOrEq:
            return LLVMRealOLE;
        case BinOpTypeCmpGreaterOrEq:
            return LLVMRealOGE;
        default:
            zig_unreachable();
    }
}

static LLVMValueRef gen_cmp_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeBinOpExpr);

    LLVMValueRef val1 = gen_expr(g, node->data.bin_op_expr.op1);
    LLVMValueRef val2 = gen_expr(g, node->data.bin_op_expr.op2);

    TypeTableEntry *op1_type = get_expr_type(node->data.bin_op_expr.op1);
    TypeTableEntry *op2_type = get_expr_type(node->data.bin_op_expr.op2);
    assert(op1_type == op2_type);

    add_debug_source_node(g, node);
    if (op1_type->id == TypeTableEntryIdFloat) {
        LLVMRealPredicate pred = cmp_op_to_real_predicate(node->data.bin_op_expr.bin_op);
        return LLVMBuildFCmp(g->builder, pred, val1, val2, "");
    } else if (op1_type->id == TypeTableEntryIdInt) {
        LLVMIntPredicate pred = cmp_op_to_int_predicate(node->data.bin_op_expr.bin_op,
                op1_type->data.integral.is_signed);
        return LLVMBuildICmp(g->builder, pred, val1, val2, "");
    } else if (op1_type->id == TypeTableEntryIdEnum) {
        LLVMIntPredicate pred = cmp_op_to_int_predicate(node->data.bin_op_expr.bin_op, false);
        return LLVMBuildICmp(g->builder, pred, val1, val2, "");
    } else {
        zig_unreachable();
    }
}

static LLVMValueRef gen_bool_and_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeBinOpExpr);

    LLVMValueRef val1 = gen_expr(g, node->data.bin_op_expr.op1);
    LLVMBasicBlockRef post_val1_block = LLVMGetInsertBlock(g->builder);

    // block for when val1 == true
    LLVMBasicBlockRef true_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "BoolAndTrue");
    // block for when val1 == false (don't even evaluate the second part)
    LLVMBasicBlockRef false_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "BoolAndFalse");

    add_debug_source_node(g, node);
    LLVMBuildCondBr(g->builder, val1, true_block, false_block);

    LLVMPositionBuilderAtEnd(g->builder, true_block);
    LLVMValueRef val2 = gen_expr(g, node->data.bin_op_expr.op2);
    LLVMBasicBlockRef post_val2_block = LLVMGetInsertBlock(g->builder);

    add_debug_source_node(g, node);
    LLVMBuildBr(g->builder, false_block);

    LLVMPositionBuilderAtEnd(g->builder, false_block);
    add_debug_source_node(g, node);
    LLVMValueRef phi = LLVMBuildPhi(g->builder, LLVMInt1Type(), "");
    LLVMValueRef incoming_values[2] = {val1, val2};
    LLVMBasicBlockRef incoming_blocks[2] = {post_val1_block, post_val2_block};
    LLVMAddIncoming(phi, incoming_values, incoming_blocks, 2);

    return phi;
}

static LLVMValueRef gen_bool_or_expr(CodeGen *g, AstNode *expr_node) {
    assert(expr_node->type == NodeTypeBinOpExpr);

    LLVMValueRef val1 = gen_expr(g, expr_node->data.bin_op_expr.op1);
    LLVMBasicBlockRef post_val1_block = LLVMGetInsertBlock(g->builder);

    // block for when val1 == false
    LLVMBasicBlockRef false_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "BoolOrFalse");
    // block for when val1 == true (don't even evaluate the second part)
    LLVMBasicBlockRef true_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "BoolOrTrue");

    add_debug_source_node(g, expr_node);
    LLVMBuildCondBr(g->builder, val1, true_block, false_block);

    LLVMPositionBuilderAtEnd(g->builder, false_block);
    LLVMValueRef val2 = gen_expr(g, expr_node->data.bin_op_expr.op2);

    LLVMBasicBlockRef post_val2_block = LLVMGetInsertBlock(g->builder);

    add_debug_source_node(g, expr_node);
    LLVMBuildBr(g->builder, true_block);

    LLVMPositionBuilderAtEnd(g->builder, true_block);
    add_debug_source_node(g, expr_node);
    LLVMValueRef phi = LLVMBuildPhi(g->builder, LLVMInt1Type(), "");
    LLVMValueRef incoming_values[2] = {val1, val2};
    LLVMBasicBlockRef incoming_blocks[2] = {post_val1_block, post_val2_block};
    LLVMAddIncoming(phi, incoming_values, incoming_blocks, 2);

    return phi;
}

static LLVMValueRef gen_struct_memcpy(CodeGen *g, AstNode *source_node, LLVMValueRef src, LLVMValueRef dest,
        TypeTableEntry *type_entry)
{
    assert(handle_is_ptr(type_entry));

    LLVMTypeRef ptr_u8 = LLVMPointerType(LLVMInt8Type(), 0);

    add_debug_source_node(g, source_node);
    LLVMValueRef src_ptr = LLVMBuildBitCast(g->builder, src, ptr_u8, "");
    LLVMValueRef dest_ptr = LLVMBuildBitCast(g->builder, dest, ptr_u8, "");

    LLVMValueRef params[] = {
        dest_ptr, // dest pointer
        src_ptr, // source pointer
        LLVMConstInt(LLVMIntType(g->pointer_size_bytes * 8), type_entry->size_in_bits / 8, false), // byte count
        LLVMConstInt(LLVMInt32Type(), type_entry->align_in_bits / 8, false), // align in bytes
        LLVMConstNull(LLVMInt1Type()), // is volatile
    };

    return LLVMBuildCall(g->builder, g->memcpy_fn_val, params, 5, "");
}

static LLVMValueRef gen_assign_raw(CodeGen *g, AstNode *source_node, BinOpType bin_op,
        LLVMValueRef target_ref, LLVMValueRef value,
        TypeTableEntry *op1_type, TypeTableEntry *op2_type)
{
    if (handle_is_ptr(op1_type)) {
        assert(op1_type == op2_type);
        assert(bin_op == BinOpTypeAssign);

        return gen_struct_memcpy(g, source_node, value, target_ref, op1_type);
    }

    if (bin_op != BinOpTypeAssign) {
        assert(source_node->type == NodeTypeBinOpExpr);
        add_debug_source_node(g, source_node->data.bin_op_expr.op1);
        LLVMValueRef left_value = LLVMBuildLoad(g->builder, target_ref, "");

        value = gen_arithmetic_bin_op(g, source_node, left_value, value, op1_type, op2_type, bin_op);
    }

    add_debug_source_node(g, source_node);
    return LLVMBuildStore(g->builder, value, target_ref);
}

static LLVMValueRef gen_assign_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeBinOpExpr);

    AstNode *lhs_node = node->data.bin_op_expr.op1;

    TypeTableEntry *op1_type;

    LLVMValueRef target_ref = gen_lvalue(g, node, lhs_node, &op1_type);

    TypeTableEntry *op2_type = get_expr_type(node->data.bin_op_expr.op2);

    LLVMValueRef value = gen_expr(g, node->data.bin_op_expr.op2);

    if (op1_type->size_in_bits == 0) {
        return nullptr;
    }

    return gen_assign_raw(g, node, node->data.bin_op_expr.bin_op, target_ref, value, op1_type, op2_type);
}

static LLVMValueRef gen_unwrap_maybe(CodeGen *g, AstNode *node, LLVMValueRef maybe_struct_ref) {
    add_debug_source_node(g, node);
    LLVMValueRef maybe_field_ptr = LLVMBuildStructGEP(g->builder, maybe_struct_ref, 0, "");
    // TODO if it's a struct we might not want to load the pointer
    return LLVMBuildLoad(g->builder, maybe_field_ptr, "");
}

static LLVMValueRef gen_unwrap_maybe_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeBinOpExpr);
    assert(node->data.bin_op_expr.bin_op == BinOpTypeUnwrapMaybe);

    AstNode *op1_node = node->data.bin_op_expr.op1;
    AstNode *op2_node = node->data.bin_op_expr.op2;

    LLVMValueRef maybe_struct_ref = gen_expr(g, op1_node);

    add_debug_source_node(g, node);
    LLVMValueRef maybe_field_ptr = LLVMBuildStructGEP(g->builder, maybe_struct_ref, 1, "");
    LLVMValueRef cond_value = LLVMBuildLoad(g->builder, maybe_field_ptr, "");

    LLVMBasicBlockRef non_null_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "MaybeNonNull");
    LLVMBasicBlockRef null_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "MaybeNull");
    LLVMBasicBlockRef end_block;

    bool non_null_reachable = get_expr_type(op1_node)->id != TypeTableEntryIdUnreachable;
    bool null_reachable = get_expr_type(op2_node)->id != TypeTableEntryIdUnreachable;
    bool end_reachable = non_null_reachable || null_reachable;
    if (end_reachable) {
        end_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "MaybeEnd");
    }

    LLVMBuildCondBr(g->builder, cond_value, non_null_block, null_block);

    LLVMPositionBuilderAtEnd(g->builder, non_null_block);
    LLVMValueRef non_null_result = gen_unwrap_maybe(g, op1_node, maybe_struct_ref);
    if (non_null_reachable) {
        add_debug_source_node(g, node);
        LLVMBuildBr(g->builder, end_block);
    }
    LLVMBasicBlockRef post_non_null_result_block = LLVMGetInsertBlock(g->builder);

    LLVMPositionBuilderAtEnd(g->builder, null_block);
    LLVMValueRef null_result = gen_expr(g, op2_node);
    if (null_reachable) {
        add_debug_source_node(g, node);
        LLVMBuildBr(g->builder, end_block);
    }
    LLVMBasicBlockRef post_null_result_block = LLVMGetInsertBlock(g->builder);

    if (end_reachable) {
        LLVMPositionBuilderAtEnd(g->builder, end_block);
        if (null_reachable) {
            add_debug_source_node(g, node);
            LLVMValueRef phi = LLVMBuildPhi(g->builder, LLVMTypeOf(non_null_result), "");
            LLVMValueRef incoming_values[2] = {non_null_result, null_result};
            LLVMBasicBlockRef incoming_blocks[2] = {post_non_null_result_block, post_null_result_block};
            LLVMAddIncoming(phi, incoming_values, incoming_blocks, 2);
            return phi;
        } else {
            return non_null_result;
        }
    }

    return nullptr;
}

static LLVMValueRef gen_bin_op_expr(CodeGen *g, AstNode *node) {
    switch (node->data.bin_op_expr.bin_op) {
        case BinOpTypeInvalid:
        case BinOpTypeStrCat:
            zig_unreachable();
        case BinOpTypeAssign:
        case BinOpTypeAssignTimes:
        case BinOpTypeAssignDiv:
        case BinOpTypeAssignMod:
        case BinOpTypeAssignPlus:
        case BinOpTypeAssignMinus:
        case BinOpTypeAssignBitShiftLeft:
        case BinOpTypeAssignBitShiftRight:
        case BinOpTypeAssignBitAnd:
        case BinOpTypeAssignBitXor:
        case BinOpTypeAssignBitOr:
        case BinOpTypeAssignBoolAnd:
        case BinOpTypeAssignBoolOr:
            return gen_assign_expr(g, node);
        case BinOpTypeBoolOr:
            return gen_bool_or_expr(g, node);
        case BinOpTypeBoolAnd:
            return gen_bool_and_expr(g, node);
        case BinOpTypeCmpEq:
        case BinOpTypeCmpNotEq:
        case BinOpTypeCmpLessThan:
        case BinOpTypeCmpGreaterThan:
        case BinOpTypeCmpLessOrEq:
        case BinOpTypeCmpGreaterOrEq:
            return gen_cmp_expr(g, node);
        case BinOpTypeUnwrapMaybe:
            return gen_unwrap_maybe_expr(g, node);
        case BinOpTypeBinOr:
        case BinOpTypeBinXor:
        case BinOpTypeBinAnd:
        case BinOpTypeBitShiftLeft:
        case BinOpTypeBitShiftRight:
        case BinOpTypeAdd:
        case BinOpTypeSub:
        case BinOpTypeMult:
        case BinOpTypeDiv:
        case BinOpTypeMod:
            return gen_arithmetic_bin_op_expr(g, node);
    }
    zig_unreachable();
}

static LLVMValueRef gen_unwrap_err_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeUnwrapErrorExpr);

    AstNode *op1 = node->data.unwrap_err_expr.op1;
    AstNode *op2 = node->data.unwrap_err_expr.op2;
    VariableTableEntry *var = node->data.unwrap_err_expr.var;

    LLVMValueRef expr_val = gen_expr(g, op1);
    TypeTableEntry *expr_type = get_expr_type(op1);
    TypeTableEntry *op2_type = get_expr_type(op2);
    assert(expr_type->id == TypeTableEntryIdErrorUnion);
    TypeTableEntry *child_type = expr_type->data.error.child_type;
    LLVMValueRef err_val;
    add_debug_source_node(g, node);
    if (handle_is_ptr(expr_type)) {
        LLVMValueRef err_val_ptr = LLVMBuildStructGEP(g->builder, expr_val, 0, "");
        err_val = LLVMBuildLoad(g->builder, err_val_ptr, "");
    } else {
        err_val = expr_val;
    }
    LLVMValueRef zero = LLVMConstNull(g->err_tag_type->type_ref);
    LLVMValueRef cond_val = LLVMBuildICmp(g->builder, LLVMIntEQ, err_val, zero, "");

    LLVMBasicBlockRef ok_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "UnwrapErrOk");
    LLVMBasicBlockRef err_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "UnwrapErrError");
    LLVMBasicBlockRef end_block;
    bool err_reachable = op2_type->id != TypeTableEntryIdUnreachable;
    bool have_end_block = err_reachable && (child_type->size_in_bits > 0);
    if (have_end_block) {
        end_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "UnwrapErrEnd");
    }

    LLVMBuildCondBr(g->builder, cond_val, ok_block, err_block);

    LLVMPositionBuilderAtEnd(g->builder, err_block);
    if (var) {
        LLVMBuildStore(g->builder, err_val, var->value_ref);
    }
    LLVMValueRef err_result = gen_expr(g, op2);
    add_debug_source_node(g, node);
    if (have_end_block) {
        LLVMBuildBr(g->builder, end_block);
    } else if (err_reachable) {
        LLVMBuildBr(g->builder, ok_block);
    }

    LLVMPositionBuilderAtEnd(g->builder, ok_block);
    if (child_type->size_in_bits == 0) {
        return nullptr;
    }
    LLVMValueRef child_val_ptr = LLVMBuildStructGEP(g->builder, expr_val, 1, "");
    LLVMValueRef child_val;
    if (handle_is_ptr(child_type)) {
        child_val = child_val_ptr;
    } else {
        child_val = LLVMBuildLoad(g->builder, child_val_ptr, "");
    }

    if (!have_end_block) {
        return child_val;
    }

    LLVMBuildBr(g->builder, end_block);

    LLVMPositionBuilderAtEnd(g->builder, end_block);
    LLVMValueRef phi = LLVMBuildPhi(g->builder, LLVMTypeOf(err_result), "");
    LLVMValueRef incoming_values[2] = {child_val, err_result};
    LLVMBasicBlockRef incoming_blocks[2] = {ok_block, err_block};
    LLVMAddIncoming(phi, incoming_values, incoming_blocks, 2);
    return phi;
}

static LLVMValueRef gen_return(CodeGen *g, AstNode *source_node, LLVMValueRef value) {
    TypeTableEntry *return_type = g->cur_fn->type_entry->data.fn.src_return_type;
    if (handle_is_ptr(return_type)) {
        assert(g->cur_ret_ptr);
        gen_assign_raw(g, source_node, BinOpTypeAssign, g->cur_ret_ptr, value, return_type, return_type);
        add_debug_source_node(g, source_node);
        return LLVMBuildRetVoid(g->builder);
    } else {
        add_debug_source_node(g, source_node);
        return LLVMBuildRet(g->builder, value);
    }
}

static LLVMValueRef gen_return_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeReturnExpr);
    AstNode *param_node = node->data.return_expr.expr;
    assert(param_node);
    LLVMValueRef value = gen_expr(g, param_node);
    TypeTableEntry *value_type = get_expr_type(param_node);

    switch (node->data.return_expr.kind) {
        case ReturnKindUnconditional:
            return gen_return(g, node, value);
        case ReturnKindError:
            {
                assert(value_type->id == TypeTableEntryIdErrorUnion);
                TypeTableEntry *child_type = value_type->data.error.child_type;

                LLVMBasicBlockRef return_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "ErrRetReturn");
                LLVMBasicBlockRef continue_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "ErrRetContinue");

                add_debug_source_node(g, node);
                LLVMValueRef err_val;
                if (child_type->size_in_bits > 0) {
                    LLVMValueRef err_val_ptr = LLVMBuildStructGEP(g->builder, value, 0, "");
                    err_val = LLVMBuildLoad(g->builder, err_val_ptr, "");
                } else {
                    err_val = value;
                }
                LLVMValueRef zero = LLVMConstNull(g->err_tag_type->type_ref);
                LLVMValueRef cond_val = LLVMBuildICmp(g->builder, LLVMIntEQ, err_val, zero, "");
                LLVMBuildCondBr(g->builder, cond_val, continue_block, return_block);

                LLVMPositionBuilderAtEnd(g->builder, return_block);
                TypeTableEntry *return_type = g->cur_fn->type_entry->data.fn.src_return_type;
                if (return_type->id == TypeTableEntryIdPureError) {
                    gen_return(g, node, err_val);
                } else if (return_type->id == TypeTableEntryIdErrorUnion) {
                    if (return_type->data.error.child_type->size_in_bits > 0) {
                        assert(g->cur_ret_ptr);

                        add_debug_source_node(g, node);
                        LLVMValueRef tag_ptr = LLVMBuildStructGEP(g->builder, g->cur_ret_ptr, 0, "");
                        LLVMBuildStore(g->builder, err_val, tag_ptr);
                        LLVMBuildRetVoid(g->builder);
                    } else {
                        gen_return(g, node, err_val);
                    }
                } else {
                    zig_unreachable();
                }

                LLVMPositionBuilderAtEnd(g->builder, continue_block);
                if (child_type->size_in_bits > 0) {
                    add_debug_source_node(g, node);
                    LLVMValueRef val_ptr = LLVMBuildStructGEP(g->builder, value, 1, "");
                    if (handle_is_ptr(child_type)) {
                        return val_ptr;
                    } else {
                        return LLVMBuildLoad(g->builder, val_ptr, "");
                    }
                } else {
                    return nullptr;
                }
            }
        case ReturnKindMaybe:
            zig_panic("TODO");
    }
}

static LLVMValueRef gen_if_bool_expr_raw(CodeGen *g, AstNode *source_node, LLVMValueRef cond_value,
        AstNode *then_node, AstNode *else_node)
{
    TypeTableEntry *then_type = get_expr_type(then_node);
    bool use_expr_value = (then_type->id != TypeTableEntryIdUnreachable &&
                           then_type->id != TypeTableEntryIdVoid);

    if (else_node) {
        LLVMBasicBlockRef then_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "Then");
        LLVMBasicBlockRef else_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "Else");

        LLVMBasicBlockRef endif_block;
        bool then_endif_reachable = get_expr_type(then_node)->id != TypeTableEntryIdUnreachable;
        bool else_endif_reachable = get_expr_type(else_node)->id != TypeTableEntryIdUnreachable;
        if (then_endif_reachable || else_endif_reachable) {
            endif_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "EndIf");
        }

        LLVMBuildCondBr(g->builder, cond_value, then_block, else_block);

        LLVMPositionBuilderAtEnd(g->builder, then_block);
        LLVMValueRef then_expr_result = gen_expr(g, then_node);
        if (then_endif_reachable) {
            LLVMBuildBr(g->builder, endif_block);
        }
        LLVMBasicBlockRef after_then_block = LLVMGetInsertBlock(g->builder);

        LLVMPositionBuilderAtEnd(g->builder, else_block);
        LLVMValueRef else_expr_result = gen_expr(g, else_node);
        if (else_endif_reachable) {
            LLVMBuildBr(g->builder, endif_block);
        }
        LLVMBasicBlockRef after_else_block = LLVMGetInsertBlock(g->builder);

        if (then_endif_reachable || else_endif_reachable) {
            LLVMPositionBuilderAtEnd(g->builder, endif_block);
            if (use_expr_value) {
                LLVMValueRef phi = LLVMBuildPhi(g->builder, LLVMTypeOf(then_expr_result), "");
                LLVMValueRef incoming_values[2] = {then_expr_result, else_expr_result};
                LLVMBasicBlockRef incoming_blocks[2] = {after_then_block, after_else_block};
                LLVMAddIncoming(phi, incoming_values, incoming_blocks, 2);

                return phi;
            }
        }

        return nullptr;
    }

    assert(!use_expr_value || then_type->id == TypeTableEntryIdErrorUnion);

    LLVMBasicBlockRef then_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "Then");
    LLVMBasicBlockRef endif_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "EndIf");

    LLVMBuildCondBr(g->builder, cond_value, then_block, endif_block);

    LLVMPositionBuilderAtEnd(g->builder, then_block);
    gen_expr(g, then_node);
    if (get_expr_type(then_node)->id != TypeTableEntryIdUnreachable)
        LLVMBuildBr(g->builder, endif_block);

    LLVMPositionBuilderAtEnd(g->builder, endif_block);

    if (use_expr_value) {
        return LLVMConstNull(g->err_tag_type->type_ref);
    } else {
        return nullptr;
    }
}

static LLVMValueRef gen_if_bool_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeIfBoolExpr);
    assert(node->data.if_bool_expr.condition);
    assert(node->data.if_bool_expr.then_block);

    ConstExprValue *const_val = &get_resolved_expr(node->data.if_bool_expr.condition)->const_val;
    if (const_val->ok) {
        if (const_val->data.x_bool) {
            return gen_expr(g, node->data.if_bool_expr.then_block);
        } else if (node->data.if_bool_expr.else_node) {
            return gen_expr(g, node->data.if_bool_expr.else_node);
        } else {
            return nullptr;
        }
    } else {
        LLVMValueRef cond_value = gen_expr(g, node->data.if_bool_expr.condition);

        return gen_if_bool_expr_raw(g, node, cond_value,
                node->data.if_bool_expr.then_block,
                node->data.if_bool_expr.else_node);
    }
}

static LLVMValueRef gen_if_var_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeIfVarExpr);
    assert(node->data.if_var_expr.var_decl.expr);

    LLVMValueRef init_val;
    gen_var_decl_raw(g, node, &node->data.if_var_expr.var_decl, true, &init_val);

    // test if value is the maybe state
    add_debug_source_node(g, node);
    LLVMValueRef maybe_field_ptr = LLVMBuildStructGEP(g->builder, init_val, 1, "");
    LLVMValueRef cond_value = LLVMBuildLoad(g->builder, maybe_field_ptr, "");

    LLVMValueRef return_value = gen_if_bool_expr_raw(g, node, cond_value,
            node->data.if_var_expr.then_block,
            node->data.if_var_expr.else_node);

    return return_value;
}

static LLVMValueRef gen_block(CodeGen *g, AstNode *block_node, TypeTableEntry *implicit_return_type) {
    assert(block_node->type == NodeTypeBlock);

    LLVMValueRef return_value;
    for (int i = 0; i < block_node->data.block.statements.length; i += 1) {
        AstNode *statement_node = block_node->data.block.statements.at(i);
        return_value = gen_expr(g, statement_node);
    }

    if (implicit_return_type && implicit_return_type->id != TypeTableEntryIdUnreachable) {
        return gen_return(g, block_node, return_value);
    } else {
        return return_value;
    }
}

static int find_asm_index(CodeGen *g, AstNode *node, AsmToken *tok) {
    const char *ptr = buf_ptr(&node->data.asm_expr.asm_template) + tok->start + 2;
    int len = tok->end - tok->start - 2;
    int result = 0;
    for (int i = 0; i < node->data.asm_expr.output_list.length; i += 1, result += 1) {
        AsmOutput *asm_output = node->data.asm_expr.output_list.at(i);
        if (buf_eql_mem(&asm_output->asm_symbolic_name, ptr, len)) {
            return result;
        }
    }
    for (int i = 0; i < node->data.asm_expr.input_list.length; i += 1, result += 1) {
        AsmInput *asm_input = node->data.asm_expr.input_list.at(i);
        if (buf_eql_mem(&asm_input->asm_symbolic_name, ptr, len)) {
            return result;
        }
    }
    return -1;
}

static LLVMValueRef gen_asm_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeAsmExpr);

    AstNodeAsmExpr *asm_expr = &node->data.asm_expr;

    Buf *src_template = &asm_expr->asm_template;

    Buf llvm_template = BUF_INIT;
    buf_resize(&llvm_template, 0);

    for (int token_i = 0; token_i < asm_expr->token_list.length; token_i += 1) {
        AsmToken *asm_token = &asm_expr->token_list.at(token_i);
        switch (asm_token->id) {
            case AsmTokenIdTemplate:
                for (int offset = asm_token->start; offset < asm_token->end; offset += 1) {
                    uint8_t c = *((uint8_t*)(buf_ptr(src_template) + offset));
                    if (c == '$') {
                        buf_append_str(&llvm_template, "$$");
                    } else {
                        buf_append_char(&llvm_template, c);
                    }
                }
                break;
            case AsmTokenIdPercent:
                buf_append_char(&llvm_template, '%');
                break;
            case AsmTokenIdVar:
                int index = find_asm_index(g, node, asm_token);
                assert(index >= 0);
                buf_appendf(&llvm_template, "$%d", index);
                break;
        }
    }

    Buf constraint_buf = BUF_INIT;
    buf_resize(&constraint_buf, 0);

    assert(asm_expr->return_count == 0 || asm_expr->return_count == 1);

    int total_constraint_count = asm_expr->output_list.length +
                                 asm_expr->input_list.length +
                                 asm_expr->clobber_list.length;
    int input_and_output_count = asm_expr->output_list.length +
                                 asm_expr->input_list.length -
                                 asm_expr->return_count;
    int total_index = 0;
    int param_index = 0;
    LLVMTypeRef *param_types = allocate<LLVMTypeRef>(input_and_output_count);
    LLVMValueRef *param_values = allocate<LLVMValueRef>(input_and_output_count);
    for (int i = 0; i < asm_expr->output_list.length; i += 1, total_index += 1) {
        AsmOutput *asm_output = asm_expr->output_list.at(i);
        bool is_return = (asm_output->return_type != nullptr);
        assert(*buf_ptr(&asm_output->constraint) == '=');
        if (is_return) {
            buf_appendf(&constraint_buf, "=%s", buf_ptr(&asm_output->constraint) + 1);
        } else {
            buf_appendf(&constraint_buf, "=*%s", buf_ptr(&asm_output->constraint) + 1);
        }
        if (total_index + 1 < total_constraint_count) {
            buf_append_char(&constraint_buf, ',');
        }

        if (!is_return) {
            VariableTableEntry *variable = find_variable( node->block_context, &asm_output->variable_name);
            assert(variable);
            param_types[param_index] = LLVMTypeOf(variable->value_ref);
            param_values[param_index] = variable->value_ref;
            param_index += 1;
        }
    }
    for (int i = 0; i < asm_expr->input_list.length; i += 1, total_index += 1, param_index += 1) {
        AsmInput *asm_input = asm_expr->input_list.at(i);
        buf_append_buf(&constraint_buf, &asm_input->constraint);
        if (total_index + 1 < total_constraint_count) {
            buf_append_char(&constraint_buf, ',');
        }

        TypeTableEntry *expr_type = get_expr_type(asm_input->expr);
        param_types[param_index] = expr_type->type_ref;
        param_values[param_index] = gen_expr(g, asm_input->expr);
    }
    for (int i = 0; i < asm_expr->clobber_list.length; i += 1, total_index += 1) {
        Buf *clobber_buf = asm_expr->clobber_list.at(i);
        buf_appendf(&constraint_buf, "~{%s}", buf_ptr(clobber_buf));
        if (total_index + 1 < total_constraint_count) {
            buf_append_char(&constraint_buf, ',');
        }
    }

    LLVMTypeRef ret_type;
    if (asm_expr->return_count == 0) {
        ret_type = LLVMVoidType();
    } else {
        ret_type = get_expr_type(node)->type_ref;
    }
    LLVMTypeRef function_type = LLVMFunctionType(ret_type, param_types, input_and_output_count, false);

    bool is_volatile = asm_expr->is_volatile || (asm_expr->output_list.length == 0);
    LLVMValueRef asm_fn = LLVMConstInlineAsm(function_type, buf_ptr(&llvm_template),
            buf_ptr(&constraint_buf), is_volatile, false);

    add_debug_source_node(g, node);
    return LLVMBuildCall(g->builder, asm_fn, param_values, input_and_output_count, "");
}

static LLVMValueRef gen_container_init_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeContainerInitExpr);

    TypeTableEntry *type_entry = get_expr_type(node);

    if (type_entry->id == TypeTableEntryIdStruct) {
        assert(node->data.container_init_expr.kind == ContainerInitKindStruct);

        int src_field_count = type_entry->data.structure.src_field_count;
        assert(src_field_count == node->data.container_init_expr.entries.length);

        StructValExprCodeGen *struct_val_expr_node = &node->data.container_init_expr.resolved_struct_val_expr;
        LLVMValueRef tmp_struct_ptr = struct_val_expr_node->ptr;

        for (int i = 0; i < src_field_count; i += 1) {
            AstNode *field_node = node->data.container_init_expr.entries.at(i);
            assert(field_node->type == NodeTypeStructValueField);
            TypeStructField *type_struct_field = field_node->data.struct_val_field.type_struct_field;
            if (type_struct_field->type_entry->id == TypeTableEntryIdVoid) {
                continue;
            }
            assert(buf_eql_buf(type_struct_field->name, &field_node->data.struct_val_field.name));

            add_debug_source_node(g, field_node);
            LLVMValueRef field_ptr = LLVMBuildStructGEP(g->builder, tmp_struct_ptr, type_struct_field->gen_index, "");
            AstNode *expr_node = field_node->data.struct_val_field.expr;
            LLVMValueRef value = gen_expr(g, expr_node);
            gen_assign_raw(g, field_node, BinOpTypeAssign, field_ptr, value,
                    type_struct_field->type_entry, get_expr_type(expr_node));
        }

        return tmp_struct_ptr;
    } else if (type_entry->id == TypeTableEntryIdUnreachable) {
        assert(node->data.container_init_expr.entries.length == 0);
        add_debug_source_node(g, node);
        return LLVMBuildUnreachable(g->builder);
    } else if (type_entry->id == TypeTableEntryIdVoid) {
        assert(node->data.container_init_expr.entries.length == 0);
        return nullptr;
    } else if (type_entry->id == TypeTableEntryIdArray) {
        StructValExprCodeGen *struct_val_expr_node = &node->data.container_init_expr.resolved_struct_val_expr;
        LLVMValueRef tmp_array_ptr = struct_val_expr_node->ptr;

        int field_count = type_entry->data.array.len;
        assert(field_count == node->data.container_init_expr.entries.length);

        TypeTableEntry *child_type = type_entry->data.array.child_type;

        for (int i = 0; i < field_count; i += 1) {
            AstNode *field_node = node->data.container_init_expr.entries.at(i);
            LLVMValueRef elem_val = gen_expr(g, field_node);

            LLVMValueRef indices[] = {
                LLVMConstNull(g->builtin_types.entry_isize->type_ref),
                LLVMConstInt(g->builtin_types.entry_isize->type_ref, i, false),
            };
            add_debug_source_node(g, field_node);
            LLVMValueRef elem_ptr = LLVMBuildInBoundsGEP(g->builder, tmp_array_ptr, indices, 2, "");
            gen_assign_raw(g, field_node, BinOpTypeAssign, elem_ptr, elem_val,
                    child_type, get_expr_type(field_node));
        }

        return tmp_array_ptr;
    } else {
        zig_unreachable();
    }
}

static LLVMValueRef gen_while_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeWhileExpr);
    assert(node->data.while_expr.condition);
    assert(node->data.while_expr.body);

    bool condition_always_true = node->data.while_expr.condition_always_true;
    bool contains_break = node->data.while_expr.contains_break;
    if (condition_always_true) {
        // generate a forever loop

        LLVMBasicBlockRef body_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "WhileBody");
        LLVMBasicBlockRef end_block = nullptr;
        if (contains_break) {
            end_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "WhileEnd");
        }

        add_debug_source_node(g, node);
        LLVMBuildBr(g->builder, body_block);

        LLVMPositionBuilderAtEnd(g->builder, body_block);
        g->break_block_stack.append(end_block);
        g->continue_block_stack.append(body_block);
        gen_expr(g, node->data.while_expr.body);
        g->break_block_stack.pop();
        g->continue_block_stack.pop();

        if (get_expr_type(node->data.while_expr.body)->id != TypeTableEntryIdUnreachable) {
            add_debug_source_node(g, node);
            LLVMBuildBr(g->builder, body_block);
        }

        if (contains_break) {
            LLVMPositionBuilderAtEnd(g->builder, end_block);
        }
    } else {
        // generate a normal while loop

        LLVMBasicBlockRef cond_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "WhileCond");
        LLVMBasicBlockRef body_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "WhileBody");
        LLVMBasicBlockRef end_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "WhileEnd");

        add_debug_source_node(g, node);
        LLVMBuildBr(g->builder, cond_block);

        LLVMPositionBuilderAtEnd(g->builder, cond_block);
        LLVMValueRef cond_val = gen_expr(g, node->data.while_expr.condition);
        add_debug_source_node(g, node->data.while_expr.condition);
        LLVMBuildCondBr(g->builder, cond_val, body_block, end_block);

        LLVMPositionBuilderAtEnd(g->builder, body_block);
        g->break_block_stack.append(end_block);
        g->continue_block_stack.append(cond_block);
        gen_expr(g, node->data.while_expr.body);
        g->break_block_stack.pop();
        g->continue_block_stack.pop();
        if (get_expr_type(node->data.while_expr.body)->id != TypeTableEntryIdUnreachable) {
            add_debug_source_node(g, node);
            LLVMBuildBr(g->builder, cond_block);
        }

        LLVMPositionBuilderAtEnd(g->builder, end_block);
    }

    return nullptr;
}

static LLVMValueRef gen_for_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeForExpr);
    assert(node->data.for_expr.array_expr);
    assert(node->data.for_expr.body);

    VariableTableEntry *elem_var = node->data.for_expr.elem_var;
    assert(elem_var);

    TypeTableEntry *array_type = get_expr_type(node->data.for_expr.array_expr);

    VariableTableEntry *index_var = node->data.for_expr.index_var;
    assert(index_var);
    LLVMValueRef index_ptr = index_var->value_ref;
    LLVMValueRef one_const = LLVMConstInt(g->builtin_types.entry_isize->type_ref, 1, false);

    LLVMBasicBlockRef cond_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "ForCond");
    LLVMBasicBlockRef body_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "ForBody");
    LLVMBasicBlockRef end_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "ForEnd");

    LLVMValueRef array_val = gen_array_base_ptr(g, node->data.for_expr.array_expr);
    add_debug_source_node(g, node);
    LLVMBuildStore(g->builder, LLVMConstNull(index_var->type->type_ref), index_ptr);
    LLVMValueRef len_val;
    TypeTableEntry *child_type;
    if (array_type->id == TypeTableEntryIdArray) {
        len_val = LLVMConstInt(g->builtin_types.entry_isize->type_ref,
                array_type->data.array.len, false);
        child_type = array_type->data.array.child_type;
    } else if (array_type->id == TypeTableEntryIdStruct) {
        assert(array_type->data.structure.is_unknown_size_array);
        TypeTableEntry *child_ptr_type = array_type->data.structure.fields[0].type_entry;
        assert(child_ptr_type->id == TypeTableEntryIdPointer);
        child_type = child_ptr_type->data.pointer.child_type;
        LLVMValueRef len_field_ptr = LLVMBuildStructGEP(g->builder, array_val, 1, "");
        len_val = LLVMBuildLoad(g->builder, len_field_ptr, "");
    } else {
        zig_unreachable();
    }
    LLVMBuildBr(g->builder, cond_block);

    LLVMPositionBuilderAtEnd(g->builder, cond_block);
    LLVMValueRef index_val = LLVMBuildLoad(g->builder, index_ptr, "");
    LLVMValueRef cond = LLVMBuildICmp(g->builder, LLVMIntSLT, index_val, len_val, "");
    LLVMBuildCondBr(g->builder, cond, body_block, end_block);

    LLVMPositionBuilderAtEnd(g->builder, body_block);
    LLVMValueRef elem_ptr = gen_array_elem_ptr(g, node, array_val, array_type, index_val);
    LLVMValueRef elem_val = handle_is_ptr(child_type) ? elem_ptr : LLVMBuildLoad(g->builder, elem_ptr, "");
    gen_assign_raw(g, node, BinOpTypeAssign, elem_var->value_ref, elem_val,
            elem_var->type, child_type);
    g->break_block_stack.append(end_block);
    g->continue_block_stack.append(cond_block);
    gen_expr(g, node->data.for_expr.body);
    g->break_block_stack.pop();
    g->continue_block_stack.pop();
    if (get_expr_type(node->data.for_expr.body)->id != TypeTableEntryIdUnreachable) {
        add_debug_source_node(g, node);
        LLVMValueRef new_index_val = LLVMBuildAdd(g->builder, index_val, one_const, "");
        LLVMBuildStore(g->builder, new_index_val, index_ptr);
        LLVMBuildBr(g->builder, cond_block);
    }

    LLVMPositionBuilderAtEnd(g->builder, end_block);
    return nullptr;
}

static LLVMValueRef gen_break(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeBreak);
    LLVMBasicBlockRef dest_block = g->break_block_stack.last();

    add_debug_source_node(g, node);
    return LLVMBuildBr(g->builder, dest_block);
}

static LLVMValueRef gen_continue(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeContinue);
    LLVMBasicBlockRef dest_block = g->continue_block_stack.last();

    add_debug_source_node(g, node);
    return LLVMBuildBr(g->builder, dest_block);
}

static LLVMValueRef gen_var_decl_raw(CodeGen *g, AstNode *source_node, AstNodeVariableDeclaration *var_decl,
        bool unwrap_maybe, LLVMValueRef *init_value)
{
    VariableTableEntry *variable = var_decl->variable;

    assert(variable);
    assert(variable->is_ptr);

    if (var_decl->expr) {
        *init_value = gen_expr(g, var_decl->expr);
    }
    if (variable->type->size_in_bits == 0) {
        return nullptr;
    }

    bool have_init_expr = false;
    if (var_decl->expr) {
        ConstExprValue *const_val = &get_resolved_expr(var_decl->expr)->const_val;
        if (!const_val->ok || !const_val->undef) {
            have_init_expr = true;
        }
    }
    if (have_init_expr) {
        TypeTableEntry *expr_type = get_expr_type(var_decl->expr);
        LLVMValueRef value;
        if (unwrap_maybe) {
            assert(var_decl->expr);
            assert(expr_type->id == TypeTableEntryIdMaybe);
            value = gen_unwrap_maybe(g, source_node, *init_value);
            expr_type = expr_type->data.maybe.child_type;
        } else {
            value = *init_value;
        }
        gen_assign_raw(g, var_decl->expr, BinOpTypeAssign, variable->value_ref,
                value, variable->type, expr_type);
    } else {
        bool ignore_uninit = false;
        TypeTableEntry *var_type = get_type_for_type_node(var_decl->type);
        if (var_type->id == TypeTableEntryIdStruct &&
            var_type->data.structure.is_unknown_size_array)
        {
            assert(var_decl->type->type == NodeTypeArrayType);
            AstNode *size_node = var_decl->type->data.array_type.size;
            if (size_node) {
                ConstExprValue *const_val = &get_resolved_expr(size_node)->const_val;
                if (!const_val->ok) {
                    TypeTableEntry *ptr_type = var_type->data.structure.fields[0].type_entry;
                    assert(ptr_type->id == TypeTableEntryIdPointer);
                    TypeTableEntry *child_type = ptr_type->data.pointer.child_type;

                    LLVMValueRef size_val = gen_expr(g, size_node);

                    add_debug_source_node(g, source_node);
                    LLVMValueRef ptr_val = LLVMBuildArrayAlloca(g->builder, child_type->type_ref,
                            size_val, "");

                    // store the freshly allocated pointer in the unknown size array struct
                    LLVMValueRef ptr_field_ptr = LLVMBuildStructGEP(g->builder,
                            variable->value_ref, 0, "");
                    LLVMBuildStore(g->builder, ptr_val, ptr_field_ptr);

                    // store the size in the len field
                    LLVMValueRef len_field_ptr = LLVMBuildStructGEP(g->builder,
                            variable->value_ref, 1, "");
                    LLVMBuildStore(g->builder, size_val, len_field_ptr);

                    // don't clobber what we just did with debug initialization
                    ignore_uninit = true;
                }
            }
        }
        if (!ignore_uninit && g->build_type != CodeGenBuildTypeRelease) {
            // memset uninitialized memory to 0xa
            add_debug_source_node(g, source_node);
            LLVMTypeRef ptr_u8 = LLVMPointerType(LLVMInt8Type(), 0);
            LLVMValueRef fill_char = LLVMConstInt(LLVMInt8Type(), 0xaa, false);
            LLVMValueRef dest_ptr = LLVMBuildBitCast(g->builder, variable->value_ref, ptr_u8, "");
            LLVMValueRef byte_count = LLVMConstInt(LLVMIntType(g->pointer_size_bytes * 8),
                    variable->type->size_in_bits / 8, false);
            LLVMValueRef align_in_bytes = LLVMConstInt(LLVMInt32Type(),
                    variable->type->align_in_bits / 8, false);
            LLVMValueRef params[] = {
                dest_ptr,
                fill_char,
                byte_count,
                align_in_bytes,
                LLVMConstNull(LLVMInt1Type()), // is volatile
            };

            LLVMBuildCall(g->builder, g->memset_fn_val, params, 5, "");
        }
    }

    LLVMZigDILocation *debug_loc = LLVMZigGetDebugLoc(source_node->line + 1, source_node->column + 1,
            source_node->block_context->di_scope);
    LLVMZigInsertDeclareAtEnd(g->dbuilder, variable->value_ref, variable->di_loc_var, debug_loc,
            LLVMGetInsertBlock(g->builder));
    return nullptr;
}

static LLVMValueRef gen_var_decl_expr(CodeGen *g, AstNode *node) {
    AstNode *init_expr = node->data.variable_declaration.expr;
    if (node->data.variable_declaration.is_const && init_expr) {
        TypeTableEntry *init_expr_type = get_expr_type(init_expr);
        if (init_expr_type->id == TypeTableEntryIdNumLitFloat ||
            init_expr_type->id == TypeTableEntryIdNumLitInt)
        {
            return nullptr;
        }
    }

    LLVMValueRef init_val;
    return gen_var_decl_raw(g, node, &node->data.variable_declaration, false, &init_val);
}

static LLVMValueRef gen_symbol(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeSymbol);
    VariableTableEntry *variable = node->data.symbol_expr.variable;
    if (variable) {
        if (variable->type->size_in_bits == 0) {
            return nullptr;
        } else if (variable->is_ptr) {
            assert(variable->value_ref);
            if (variable->type->id == TypeTableEntryIdArray) {
                return variable->value_ref;
            } else if (variable->type->id == TypeTableEntryIdStruct ||
                        variable->type->id == TypeTableEntryIdMaybe)
            {
                return variable->value_ref;
            } else {
                add_debug_source_node(g, node);
                return LLVMBuildLoad(g->builder, variable->value_ref, "");
            }
        } else {
            return variable->value_ref;
        }
    }

    FnTableEntry *fn_entry = node->data.symbol_expr.fn_entry;
    assert(fn_entry);
    return fn_entry->fn_value;
}

static LLVMValueRef gen_switch_expr(CodeGen *g, AstNode *node) {
    assert(node->type == NodeTypeSwitchExpr);

    LLVMValueRef target_value = gen_expr(g, node->data.switch_expr.expr);

    bool end_unreachable = (get_expr_type(node)->id == TypeTableEntryIdUnreachable);

    LLVMBasicBlockRef end_block = end_unreachable ?
        nullptr : LLVMAppendBasicBlock(g->cur_fn->fn_value, "SwitchEnd");
    LLVMBasicBlockRef else_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "SwitchElse");
    int prong_count = node->data.switch_expr.prongs.length;

    add_debug_source_node(g, node);
    LLVMValueRef switch_instr = LLVMBuildSwitch(g->builder, target_value, else_block, prong_count);

    ZigList<LLVMValueRef> incoming_values = {0};
    ZigList<LLVMBasicBlockRef> incoming_blocks = {0};

    AstNode *else_prong = nullptr;
    for (int prong_i = 0; prong_i < prong_count; prong_i += 1) {
        AstNode *prong_node = node->data.switch_expr.prongs.at(prong_i);
        LLVMBasicBlockRef prong_block;
        if (prong_node->data.switch_prong.items.length == 0) {
            assert(!else_prong);
            else_prong = prong_node;
            prong_block = else_block;
        } else {
            prong_block = LLVMAppendBasicBlock(g->cur_fn->fn_value, "SwitchProng");
            for (int item_i = 0; item_i < prong_node->data.switch_prong.items.length; item_i += 1) {
                AstNode *item_node = prong_node->data.switch_prong.items.at(item_i);
                assert(item_node->type != NodeTypeSwitchRange);
                assert(get_resolved_expr(item_node)->const_val.ok);
                LLVMValueRef val = gen_expr(g, item_node);
                LLVMAddCase(switch_instr, val, prong_block);
            }
        }
        assert(!prong_node->data.switch_prong.var_symbol);
        LLVMPositionBuilderAtEnd(g->builder, prong_block);
        AstNode *prong_expr = prong_node->data.switch_prong.expr;
        LLVMValueRef prong_val = gen_expr(g, prong_expr);

        if (get_expr_type(prong_expr)->id != TypeTableEntryIdUnreachable) {
            add_debug_source_node(g, prong_expr);
            LLVMBuildBr(g->builder, end_block);
            incoming_values.append(prong_val);
            incoming_blocks.append(prong_block);
        }
    }

    if (!else_prong) {
        LLVMPositionBuilderAtEnd(g->builder, else_block);
        add_debug_source_node(g, node);
        LLVMBuildUnreachable(g->builder);
    }

    if (end_unreachable) {
        return nullptr;
    }

    LLVMPositionBuilderAtEnd(g->builder, end_block);

    add_debug_source_node(g, node);
    LLVMValueRef phi = LLVMBuildPhi(g->builder, LLVMTypeOf(incoming_values.at(0)), "");
    LLVMAddIncoming(phi, incoming_values.items, incoming_blocks.items, incoming_values.length);

    return phi;
}

static LLVMValueRef gen_expr(CodeGen *g, AstNode *node) {
    Expr *expr = get_resolved_expr(node);
    if (expr->const_val.ok) {
        if (expr->type_entry->size_in_bits == 0) {
            return nullptr;
        } else {
            assert(expr->const_llvm_val);
            return expr->const_llvm_val;
        }
    }
    switch (node->type) {
        case NodeTypeBinOpExpr:
            return gen_bin_op_expr(g, node);
        case NodeTypeUnwrapErrorExpr:
            return gen_unwrap_err_expr(g, node);
        case NodeTypeReturnExpr:
            return gen_return_expr(g, node);
        case NodeTypeVariableDeclaration:
            return gen_var_decl_expr(g, node);
        case NodeTypePrefixOpExpr:
            return gen_prefix_op_expr(g, node);
        case NodeTypeFnCallExpr:
            return gen_fn_call_expr(g, node);
        case NodeTypeArrayAccessExpr:
            return gen_array_access_expr(g, node, false);
        case NodeTypeSliceExpr:
            return gen_slice_expr(g, node);
        case NodeTypeFieldAccessExpr:
            return gen_field_access_expr(g, node, false);
        case NodeTypeIfBoolExpr:
            return gen_if_bool_expr(g, node);
        case NodeTypeIfVarExpr:
            return gen_if_var_expr(g, node);
        case NodeTypeWhileExpr:
            return gen_while_expr(g, node);
        case NodeTypeForExpr:
            return gen_for_expr(g, node);
        case NodeTypeAsmExpr:
            return gen_asm_expr(g, node);
        case NodeTypeSymbol:
            return gen_symbol(g, node);
        case NodeTypeBlock:
            return gen_block(g, node, nullptr);
        case NodeTypeGoto:
            add_debug_source_node(g, node);
            return LLVMBuildBr(g->builder, node->data.goto_expr.label_entry->basic_block);
        case NodeTypeBreak:
            return gen_break(g, node);
        case NodeTypeContinue:
            return gen_continue(g, node);
        case NodeTypeLabel:
            {
                LabelTableEntry *label_entry = node->data.label.label_entry;
                assert(label_entry);
                LLVMBasicBlockRef basic_block = label_entry->basic_block;
                if (label_entry->entered_from_fallthrough) {
                    add_debug_source_node(g, node);
                    LLVMBuildBr(g->builder, basic_block);
                }
                LLVMPositionBuilderAtEnd(g->builder, basic_block);
                return nullptr;
            }
        case NodeTypeContainerInitExpr:
            return gen_container_init_expr(g, node);
        case NodeTypeSwitchExpr:
            return gen_switch_expr(g, node);
        case NodeTypeNumberLiteral:
        case NodeTypeBoolLiteral:
        case NodeTypeStringLiteral:
        case NodeTypeCharLiteral:
        case NodeTypeNullLiteral:
        case NodeTypeUndefinedLiteral:
            // caught by constant expression eval codegen
            zig_unreachable();
        case NodeTypeRoot:
        case NodeTypeRootExportDecl:
        case NodeTypeFnProto:
        case NodeTypeFnDef:
        case NodeTypeFnDecl:
        case NodeTypeParamDecl:
        case NodeTypeDirective:
        case NodeTypeImport:
        case NodeTypeCImport:
        case NodeTypeStructDecl:
        case NodeTypeStructField:
        case NodeTypeStructValueField:
        case NodeTypeArrayType:
        case NodeTypeErrorType:
        case NodeTypeSwitchProng:
        case NodeTypeSwitchRange:
        case NodeTypeErrorValueDecl:
            zig_unreachable();
    }
    zig_unreachable();
}

static void build_label_blocks(CodeGen *g, AstNode *block_node) {
    assert(block_node->type == NodeTypeBlock);
    for (int i = 0; i < block_node->data.block.statements.length; i += 1) {
        AstNode *label_node = block_node->data.block.statements.at(i);
        if (label_node->type != NodeTypeLabel)
            continue;

        Buf *name = &label_node->data.label.name;
        label_node->data.label.label_entry->basic_block = LLVMAppendBasicBlock(
                g->cur_fn->fn_value, buf_ptr(name));
    }
}

static LLVMValueRef gen_const_val(CodeGen *g, TypeTableEntry *type_entry, ConstExprValue *const_val) {
    assert(const_val->ok);

    if (const_val->undef) {
        return LLVMGetUndef(type_entry->type_ref);
    }

    switch (type_entry->id) {
        case TypeTableEntryIdInt:
            return LLVMConstInt(type_entry->type_ref, bignum_to_twos_complement(&const_val->data.x_bignum), false);
        case TypeTableEntryIdPureError:
            assert(const_val->data.x_err.err);
            return LLVMConstInt(g->builtin_types.entry_pure_error->type_ref,
                    const_val->data.x_err.err->value, false);
        case TypeTableEntryIdFloat:
            if (const_val->data.x_bignum.kind == BigNumKindFloat) {
                return LLVMConstReal(type_entry->type_ref, const_val->data.x_bignum.data.x_float);
            } else {
                int64_t x = const_val->data.x_bignum.data.x_uint;
                if (const_val->data.x_bignum.is_negative) {
                    x = -x;
                }
                return LLVMConstReal(type_entry->type_ref, x);
            }
        case TypeTableEntryIdBool:
            if (const_val->data.x_bool) {
                return LLVMConstAllOnes(LLVMInt1Type());
            } else {
                return LLVMConstNull(LLVMInt1Type());
            }
        case TypeTableEntryIdMaybe:
            {
                TypeTableEntry *child_type = type_entry->data.maybe.child_type;
                LLVMValueRef child_val;
                LLVMValueRef maybe_val;
                if (const_val->data.x_maybe) {
                    child_val = gen_const_val(g, child_type, const_val->data.x_maybe);
                    maybe_val = LLVMConstAllOnes(LLVMInt1Type());
                } else {
                    child_val = LLVMConstNull(child_type->type_ref);
                    maybe_val = LLVMConstNull(LLVMInt1Type());
                }
                LLVMValueRef fields[] = {
                    child_val,
                    maybe_val,
                };
                return LLVMConstStruct(fields, 2, false);
            }
        case TypeTableEntryIdStruct:
            {
                LLVMValueRef *fields = allocate<LLVMValueRef>(type_entry->data.structure.gen_field_count);
                for (int i = 0; i < type_entry->data.structure.src_field_count; i += 1) {
                    TypeStructField *type_struct_field = &type_entry->data.structure.fields[i];
                    if (type_struct_field->gen_index == -1) {
                        continue;
                    }
                    fields[type_struct_field->gen_index] = gen_const_val(g, type_struct_field->type_entry,
                            const_val->data.x_struct.fields[i]);
                }
                return LLVMConstNamedStruct(type_entry->type_ref, fields,
                        type_entry->data.structure.gen_field_count);
            }
        case TypeTableEntryIdArray:
            {
                TypeTableEntry *child_type = type_entry->data.array.child_type;
                uint64_t len = type_entry->data.array.len;
                LLVMValueRef *values = allocate<LLVMValueRef>(len);
                for (int i = 0; i < len; i += 1) {
                    ConstExprValue *field_value = const_val->data.x_array.fields[i];
                    values[i] = gen_const_val(g, child_type, field_value);
                }
                return LLVMConstArray(child_type->type_ref, values, len);
            }
        case TypeTableEntryIdEnum:
            {
                LLVMTypeRef tag_type_ref = type_entry->data.enumeration.tag_type->type_ref;
                LLVMValueRef tag_value = LLVMConstInt(tag_type_ref, const_val->data.x_enum.tag, false);
                if (type_entry->data.enumeration.gen_field_count == 0) {
                    return tag_value;
                } else {
                    zig_panic("TODO");
                }
            }
        case TypeTableEntryIdFn:
            return const_val->data.x_fn->fn_value;
        case TypeTableEntryIdPointer:
            {
                TypeTableEntry *child_type = type_entry->data.pointer.child_type;
                int len = const_val->data.x_ptr.len;
                LLVMValueRef target_val;
                if (len == 1) {
                    target_val = gen_const_val(g, child_type, const_val->data.x_ptr.ptr[0]);
                } else if (len > 1) {
                    LLVMValueRef *values = allocate<LLVMValueRef>(len);
                    for (int i = 0; i < len; i += 1) {
                        values[i] = gen_const_val(g, child_type, const_val->data.x_ptr.ptr[i]);
                    }
                    target_val = LLVMConstArray(child_type->type_ref, values, len);
                } else {
                    zig_unreachable();
                }
                LLVMValueRef global_value = LLVMAddGlobal(g->module, LLVMTypeOf(target_val), "");
                LLVMSetInitializer(global_value, target_val);
                LLVMSetLinkage(global_value, LLVMPrivateLinkage);
                LLVMSetGlobalConstant(global_value, type_entry->data.pointer.is_const);
                LLVMSetUnnamedAddr(global_value, true);

                if (len > 1) {
                    return LLVMConstBitCast(global_value, type_entry->type_ref);
                } else {
                    return global_value;
                }
            }
        case TypeTableEntryIdErrorUnion:
            {
                TypeTableEntry *child_type = type_entry->data.error.child_type;
                if (child_type->size_in_bits == 0) {
                    uint64_t value = const_val->data.x_err.err ? const_val->data.x_err.err->value : 0;
                    return LLVMConstInt(g->err_tag_type->type_ref, value, false);
                } else {
                    LLVMValueRef err_tag_value;
                    LLVMValueRef err_payload_value;
                    if (const_val->data.x_err.err) {
                        err_tag_value = LLVMConstInt(g->err_tag_type->type_ref, const_val->data.x_err.err->value, false);
                        err_payload_value = LLVMConstNull(child_type->type_ref);
                    } else {
                        err_tag_value = LLVMConstNull(g->err_tag_type->type_ref);
                        err_payload_value = gen_const_val(g, child_type, const_val->data.x_err.payload);
                    }
                    LLVMValueRef fields[] = {
                        err_tag_value,
                        err_payload_value,
                    };
                    return LLVMConstStruct(fields, 2, false);
                }
            }
        case TypeTableEntryIdInvalid:
        case TypeTableEntryIdMetaType:
        case TypeTableEntryIdUnreachable:
        case TypeTableEntryIdNumLitFloat:
        case TypeTableEntryIdNumLitInt:
        case TypeTableEntryIdUndefLit:
        case TypeTableEntryIdVoid:
            zig_unreachable();

    }
}

static void gen_const_globals(CodeGen *g) {
    for (int i = 0; i < g->global_const_list.length; i += 1) {
        Expr *expr = g->global_const_list.at(i);
        ConstExprValue *const_val = &expr->const_val;
        assert(const_val->ok);
        TypeTableEntry *type_entry = expr->type_entry;

        if (handle_is_ptr(type_entry)) {
            LLVMValueRef init_val = gen_const_val(g, type_entry, const_val);
            LLVMValueRef global_value = LLVMAddGlobal(g->module, LLVMTypeOf(init_val), "");
            LLVMSetInitializer(global_value, init_val);
            LLVMSetLinkage(global_value, LLVMPrivateLinkage);
            LLVMSetGlobalConstant(global_value, true);
            LLVMSetUnnamedAddr(global_value, true);
            expr->const_llvm_val = global_value;
        } else {
            expr->const_llvm_val = gen_const_val(g, type_entry, const_val);
        }
    }
}

static void do_code_gen(CodeGen *g) {
    assert(!g->errors.length);

    gen_const_globals(g);

    // Generate module level variables
    for (int i = 0; i < g->global_vars.length; i += 1) {
        VariableTableEntry *var = g->global_vars.at(i);

        if (var->type->id == TypeTableEntryIdNumLitFloat ||
            var->type->id == TypeTableEntryIdNumLitInt ||
            var->type->size_in_bits == 0)
        {
            continue;
        }

        // TODO if the global is exported, set external linkage
        LLVMValueRef init_val;

        assert(var->decl_node);
        assert(var->decl_node->type == NodeTypeVariableDeclaration);
        AstNode *expr_node = var->decl_node->data.variable_declaration.expr;
        if (expr_node) {
            Expr *expr = get_resolved_expr(expr_node);
            ConstExprValue *const_val = &expr->const_val;
            assert(const_val->ok);
            TypeTableEntry *type_entry = expr->type_entry;
            init_val = gen_const_val(g, type_entry, const_val);
        } else {
            init_val = LLVMConstNull(var->type->type_ref);
        }
        LLVMValueRef global_value = LLVMAddGlobal(g->module, LLVMTypeOf(init_val), buf_ptr(&var->name));
        LLVMSetInitializer(global_value, init_val);
        LLVMSetGlobalConstant(global_value, var->is_const);
        LLVMSetUnnamedAddr(global_value, true);
        LLVMSetLinkage(global_value, LLVMInternalLinkage);

        var->value_ref = global_value;
    }

    // Generate function prototypes
    for (int fn_proto_i = 0; fn_proto_i < g->fn_protos.length; fn_proto_i += 1) {
        FnTableEntry *fn_table_entry = g->fn_protos.at(fn_proto_i);
        AstNode *proto_node = fn_table_entry->proto_node;
        assert(proto_node->type == NodeTypeFnProto);
        AstNodeFnProto *fn_proto = &proto_node->data.fn_proto;

        if (handle_is_ptr(fn_table_entry->type_entry->data.fn.src_return_type)) {
            LLVMValueRef first_arg = LLVMGetParam(fn_table_entry->fn_value, 0);
            LLVMAddAttribute(first_arg, LLVMStructRetAttribute);
        }

        // set parameter attributes
        for (int param_decl_i = 0; param_decl_i < fn_proto->params.length; param_decl_i += 1) {
            AstNode *param_node = fn_proto->params.at(param_decl_i);
            assert(param_node->type == NodeTypeParamDecl);

            int gen_index = param_node->data.param_decl.gen_index;

            if (gen_index < 0) {
                continue;
            }

            AstNode *type_node = param_node->data.param_decl.type;
            TypeTableEntry *param_type = fn_proto_type_from_type_node(g, type_node);
            LLVMValueRef argument_val = LLVMGetParam(fn_table_entry->fn_value, gen_index);
            bool param_is_noalias = param_node->data.param_decl.is_noalias;
            if (param_type->id == TypeTableEntryIdPointer && param_is_noalias) {
                LLVMAddAttribute(argument_val, LLVMNoAliasAttribute);
            }
            if (param_type->id == TypeTableEntryIdPointer && param_type->data.pointer.is_const) {
                LLVMAddAttribute(argument_val, LLVMReadOnlyAttribute);
            }
            if (param_type->id == TypeTableEntryIdPointer) {
                // when https://github.com/andrewrk/zig/issues/82 is fixed, add
                // non null attribute here
            }
            if (param_node->data.param_decl.is_byval) {
                LLVMAddAttribute(argument_val, LLVMByValAttribute);
            }
        }

    }

    // Generate function definitions.
    for (int fn_i = 0; fn_i < g->fn_defs.length; fn_i += 1) {
        FnTableEntry *fn_table_entry = g->fn_defs.at(fn_i);
        ImportTableEntry *import = fn_table_entry->import_entry;
        AstNode *fn_def_node = fn_table_entry->fn_def_node;
        LLVMValueRef fn = fn_table_entry->fn_value;
        g->cur_fn = fn_table_entry;
        if (handle_is_ptr(fn_table_entry->type_entry->data.fn.src_return_type)) {
            g->cur_ret_ptr = LLVMGetParam(fn, 0);
        } else {
            g->cur_ret_ptr = nullptr;
        }

        AstNode *proto_node = fn_table_entry->proto_node;
        assert(proto_node->type == NodeTypeFnProto);
        AstNodeFnProto *fn_proto = &proto_node->data.fn_proto;

        LLVMBasicBlockRef entry_block = LLVMAppendBasicBlock(fn, "entry");
        LLVMPositionBuilderAtEnd(g->builder, entry_block);


        AstNode *body_node = fn_def_node->data.fn_def.body;
        build_label_blocks(g, body_node);

        // Set up debug info for blocks and variables and
        // allocate all local variables
        for (int bc_i = 0; bc_i < fn_table_entry->all_block_contexts.length; bc_i += 1) {
            BlockContext *block_context = fn_table_entry->all_block_contexts.at(bc_i);

            if (!block_context->di_scope) {
                LLVMZigDILexicalBlock *di_block = LLVMZigCreateLexicalBlock(g->dbuilder,
                    block_context->parent->di_scope,
                    import->di_file,
                    block_context->node->line + 1,
                    block_context->node->column + 1);
                block_context->di_scope = LLVMZigLexicalBlockToScope(di_block);
            }

            for (int var_i = 0; var_i < block_context->variable_list.length; var_i += 1) {
                VariableTableEntry *var = block_context->variable_list.at(var_i);

                if (var->type->size_in_bits == 0) {
                    continue;
                }

                unsigned tag;
                unsigned arg_no;
                if (block_context->node->type == NodeTypeFnDef) {
                    tag = LLVMZigTag_DW_arg_variable();
                    arg_no = var->gen_arg_index + 1;

                    var->is_ptr = false;
                    var->value_ref = LLVMGetParam(fn, var->gen_arg_index);
                } else {
                    tag = LLVMZigTag_DW_auto_variable();
                    arg_no = 0;

                    add_debug_source_node(g, var->decl_node);
                    var->value_ref = LLVMBuildAlloca(g->builder, var->type->type_ref, buf_ptr(&var->name));
                    LLVMSetAlignment(var->value_ref, var->type->align_in_bits / 8);
                }

                var->di_loc_var = LLVMZigCreateLocalVariable(g->dbuilder, tag,
                        block_context->di_scope, buf_ptr(&var->name),
                        import->di_file, var->decl_node->line + 1,
                        var->type->di_type, !g->strip_debug_symbols, 0, arg_no);
            }

            // allocate structs which are the result of casts
            for (int cea_i = 0; cea_i < block_context->cast_alloca_list.length; cea_i += 1) {
                AstNode *fn_call_node = block_context->cast_alloca_list.at(cea_i);
                add_debug_source_node(g, fn_call_node);
                Expr *expr = &fn_call_node->data.fn_call_expr.resolved_expr;
                fn_call_node->data.fn_call_expr.tmp_ptr = LLVMBuildAlloca(g->builder,
                        expr->type_entry->type_ref, "");
            }

            // allocate structs which are struct value expressions
            for (int alloca_i = 0; alloca_i < block_context->struct_val_expr_alloca_list.length; alloca_i += 1) {
                StructValExprCodeGen *struct_val_expr_node = block_context->struct_val_expr_alloca_list.at(alloca_i);
                add_debug_source_node(g, struct_val_expr_node->source_node);
                struct_val_expr_node->ptr = LLVMBuildAlloca(g->builder,
                        struct_val_expr_node->type_entry->type_ref, "");
            }
        }

        // create debug variable declarations for parameters
        for (int param_i = 0; param_i < fn_proto->params.length; param_i += 1) {
            AstNode *param_decl = fn_proto->params.at(param_i);
            assert(param_decl->type == NodeTypeParamDecl);

            if (param_decl->data.param_decl.gen_index < 0) {
                continue;
            }

            VariableTableEntry *variable = param_decl->data.param_decl.variable;

            LLVMZigDILocation *debug_loc = LLVMZigGetDebugLoc(param_decl->line + 1, param_decl->column + 1,
                    fn_def_node->data.fn_def.block_context->di_scope);
            LLVMZigInsertDeclareAtEnd(g->dbuilder, variable->value_ref, variable->di_loc_var, debug_loc,
                    entry_block);
        }

        TypeTableEntry *implicit_return_type = fn_def_node->data.fn_def.implicit_return_type;
        gen_block(g, fn_def_node->data.fn_def.body, implicit_return_type);

    }
    assert(!g->errors.length);

    LLVMZigDIBuilderFinalize(g->dbuilder);

    if (g->verbose) {
        LLVMDumpModule(g->module);
    }

    // in release mode, we're sooooo confident that we've generated correct ir,
    // that we skip the verify module step in order to get better performance.
#ifndef NDEBUG
    char *error = nullptr;
    LLVMVerifyModule(g->module, LLVMAbortProcessAction, &error);
#endif
}

static const int int_sizes_in_bits[] = {
    8,
    16,
    32,
    64,
};

enum CIntType {
    CIntTypeShort,
    CIntTypeUShort,
    CIntTypeInt,
    CIntTypeUInt,
    CIntTypeLong,
    CIntTypeULong,
    CIntTypeLongLong,
    CIntTypeULongLong,
};

struct CIntTypeInfo {
    CIntType id;
    const char *name;
    bool is_signed;
};

static const CIntTypeInfo c_int_type_infos[] = {
    {CIntTypeShort, "c_short", true},
    {CIntTypeUShort, "c_ushort", false},
    {CIntTypeInt, "c_int", true},
    {CIntTypeUInt, "c_uint", false},
    {CIntTypeLong, "c_long", true},
    {CIntTypeULong, "c_ulong", false},
    {CIntTypeLongLong, "c_longlong", true},
    {CIntTypeULongLong, "c_ulonglong", false},
};

static int get_c_type_size_in_bits(CodeGen *g, CIntType id) {
    // TODO other architectures besides x86_64
    switch (id) {
        case CIntTypeShort:
        case CIntTypeUShort:
            return 16;
        case CIntTypeInt:
        case CIntTypeUInt:
            return 32;
        case CIntTypeLong:
        case CIntTypeULong:
        case CIntTypeLongLong:
        case CIntTypeULongLong:
            return 64;
    }
    zig_unreachable();
}

static void define_builtin_types(CodeGen *g) {
    {
        // if this type is anywhere in the AST, we should never hit codegen.
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdInvalid);
        buf_init_from_str(&entry->name, "(invalid)");
        g->builtin_types.entry_invalid = entry;
    }
    {
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdNumLitFloat);
        buf_init_from_str(&entry->name, "(float literal)");
        g->builtin_types.entry_num_lit_float = entry;
    }
    {
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdNumLitInt);
        buf_init_from_str(&entry->name, "(integer literal)");
        g->builtin_types.entry_num_lit_int = entry;
    }
    {
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdUndefLit);
        buf_init_from_str(&entry->name, "(undefined)");
        g->builtin_types.entry_undef = entry;
    }

    for (int i = 0; i < array_length(int_sizes_in_bits); i += 1) {
        int size_in_bits = int_sizes_in_bits[i];
        bool is_signed = true;
        for (;;) {
            TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdInt);
            entry->type_ref = LLVMIntType(size_in_bits);

            const char u_or_i = is_signed ? 'i' : 'u';
            buf_resize(&entry->name, 0);
            buf_appendf(&entry->name, "%c%d", u_or_i, size_in_bits);

            entry->size_in_bits = size_in_bits;
            entry->align_in_bits = size_in_bits;
            entry->di_type = LLVMZigCreateDebugBasicType(g->dbuilder, buf_ptr(&entry->name),
                    entry->size_in_bits, entry->align_in_bits,
                    is_signed ? LLVMZigEncoding_DW_ATE_signed() : LLVMZigEncoding_DW_ATE_unsigned());
            entry->data.integral.is_signed = is_signed;
            g->primitive_type_table.put(&entry->name, entry);

            get_int_type_ptr(g, is_signed, size_in_bits)[0] = entry;

            if (!is_signed) {
                break;
            } else {
                is_signed = false;
            }
        }
    }

    for (int i = 0; i < array_length(c_int_type_infos); i += 1) {
        const CIntTypeInfo *info = &c_int_type_infos[i];
        uint64_t size_in_bits = get_c_type_size_in_bits(g, info->id);
        bool is_signed = info->is_signed;

        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdInt);
        entry->type_ref = LLVMIntType(size_in_bits);

        buf_init_from_str(&entry->name, info->name);

        entry->size_in_bits = size_in_bits;
        entry->align_in_bits = size_in_bits;

        entry->di_type = LLVMZigCreateDebugBasicType(g->dbuilder, buf_ptr(&entry->name),
                entry->size_in_bits, entry->align_in_bits,
                is_signed ? LLVMZigEncoding_DW_ATE_signed() : LLVMZigEncoding_DW_ATE_unsigned());
        entry->data.integral.is_signed = is_signed;
        g->primitive_type_table.put(&entry->name, entry);
    }

    {
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdBool);
        entry->type_ref = LLVMInt1Type();
        buf_init_from_str(&entry->name, "bool");
        entry->size_in_bits = 8;
        entry->align_in_bits = 8;
        entry->di_type = LLVMZigCreateDebugBasicType(g->dbuilder, buf_ptr(&entry->name),
                entry->size_in_bits, entry->align_in_bits,
                LLVMZigEncoding_DW_ATE_unsigned());
        g->builtin_types.entry_bool = entry;
        g->primitive_type_table.put(&entry->name, entry);
    }
    {
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdInt);
        entry->type_ref = LLVMIntType(g->pointer_size_bytes * 8);
        buf_init_from_str(&entry->name, "isize");
        entry->size_in_bits = g->pointer_size_bytes * 8;
        entry->align_in_bits = g->pointer_size_bytes * 8;
        entry->data.integral.is_signed = true;

        entry->di_type = LLVMZigCreateDebugBasicType(g->dbuilder, buf_ptr(&entry->name),
                entry->size_in_bits, entry->align_in_bits,
                LLVMZigEncoding_DW_ATE_signed());
        g->builtin_types.entry_isize = entry;
        g->primitive_type_table.put(&entry->name, entry);
    }
    {
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdInt);
        entry->type_ref = LLVMIntType(g->pointer_size_bytes * 8);
        buf_init_from_str(&entry->name, "usize");
        entry->size_in_bits = g->pointer_size_bytes * 8;
        entry->align_in_bits = g->pointer_size_bytes * 8;
        entry->data.integral.is_signed = false;

        entry->di_type = LLVMZigCreateDebugBasicType(g->dbuilder, buf_ptr(&entry->name),
                entry->size_in_bits, entry->align_in_bits,
                LLVMZigEncoding_DW_ATE_unsigned());
        g->builtin_types.entry_usize = entry;
        g->primitive_type_table.put(&entry->name, entry);
    }
    {
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdFloat);
        entry->type_ref = LLVMFloatType();
        buf_init_from_str(&entry->name, "f32");
        entry->size_in_bits = 32;
        entry->align_in_bits = 32;
        entry->di_type = LLVMZigCreateDebugBasicType(g->dbuilder, buf_ptr(&entry->name),
                entry->size_in_bits, entry->align_in_bits,
                LLVMZigEncoding_DW_ATE_float());
        g->builtin_types.entry_f32 = entry;
        g->primitive_type_table.put(&entry->name, entry);
    }
    {
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdFloat);
        entry->type_ref = LLVMDoubleType();
        buf_init_from_str(&entry->name, "f64");
        entry->size_in_bits = 64;
        entry->align_in_bits = 64;
        entry->di_type = LLVMZigCreateDebugBasicType(g->dbuilder, buf_ptr(&entry->name),
                entry->size_in_bits, entry->align_in_bits,
                LLVMZigEncoding_DW_ATE_float());
        g->builtin_types.entry_f64 = entry;
        g->primitive_type_table.put(&entry->name, entry);
    }
    {
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdVoid);
        entry->type_ref = LLVMVoidType();
        buf_init_from_str(&entry->name, "void");
        entry->di_type = LLVMZigCreateDebugBasicType(g->dbuilder, buf_ptr(&entry->name),
                entry->size_in_bits, entry->align_in_bits,
                LLVMZigEncoding_DW_ATE_unsigned());
        g->builtin_types.entry_void = entry;
        g->primitive_type_table.put(&entry->name, entry);
    }
    {
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdUnreachable);
        entry->type_ref = LLVMVoidType();
        buf_init_from_str(&entry->name, "unreachable");
        entry->di_type = g->builtin_types.entry_void->di_type;
        g->builtin_types.entry_unreachable = entry;
        g->primitive_type_table.put(&entry->name, entry);
    }
    {
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdMetaType);
        buf_init_from_str(&entry->name, "type");
        g->builtin_types.entry_type = entry;
        g->primitive_type_table.put(&entry->name, entry);
    }
    {
        // partially complete the error type. we complete it later after we know
        // error_value_count.
        TypeTableEntry *entry = new_type_table_entry(TypeTableEntryIdPureError);
        buf_init_from_str(&entry->name, "error");
        g->builtin_types.entry_pure_error = entry;
        g->primitive_type_table.put(&entry->name, entry);
    }

    g->builtin_types.entry_u8 = get_int_type(g, false, 8);
    g->builtin_types.entry_u16 = get_int_type(g, false, 16);
    g->builtin_types.entry_u32 = get_int_type(g, false, 32);
    g->builtin_types.entry_u64 = get_int_type(g, false, 64);
    g->builtin_types.entry_i8 = get_int_type(g, true, 8);
    g->builtin_types.entry_i16 = get_int_type(g, true, 16);
    g->builtin_types.entry_i32 = get_int_type(g, true, 32);
    g->builtin_types.entry_i64 = get_int_type(g, true, 64);
}


static BuiltinFnEntry *create_builtin_fn(CodeGen *g, BuiltinFnId id, const char *name) {
    BuiltinFnEntry *builtin_fn = allocate<BuiltinFnEntry>(1);
    buf_init_from_str(&builtin_fn->name, name);
    builtin_fn->id = id;
    g->builtin_fn_table.put(&builtin_fn->name, builtin_fn);
    return builtin_fn;
}

static BuiltinFnEntry *create_builtin_fn_with_arg_count(CodeGen *g, BuiltinFnId id, const char *name, int count) {
    BuiltinFnEntry *builtin_fn = create_builtin_fn(g, id, name);
    builtin_fn->param_count = count;
    builtin_fn->param_types = allocate<TypeTableEntry *>(count);
    return builtin_fn;
}

static void define_builtin_fns(CodeGen *g) {
    {
        BuiltinFnEntry *builtin_fn = create_builtin_fn(g, BuiltinFnIdMemcpy, "memcpy");
        builtin_fn->return_type = g->builtin_types.entry_void;
        builtin_fn->param_count = 3;
        builtin_fn->param_types = allocate<TypeTableEntry *>(builtin_fn->param_count);
        builtin_fn->param_types[0] = nullptr; // manually checked later
        builtin_fn->param_types[1] = nullptr; // manually checked later
        builtin_fn->param_types[2] = g->builtin_types.entry_isize;

        LLVMTypeRef param_types[] = {
            LLVMPointerType(LLVMInt8Type(), 0),
            LLVMPointerType(LLVMInt8Type(), 0),
            LLVMIntType(g->pointer_size_bytes * 8),
            LLVMInt32Type(),
            LLVMInt1Type(),
        };
        LLVMTypeRef fn_type = LLVMFunctionType(LLVMVoidType(), param_types, 5, false);
        Buf *name = buf_sprintf("llvm.memcpy.p0i8.p0i8.i%d", g->pointer_size_bytes * 8);
        builtin_fn->fn_val = LLVMAddFunction(g->module, buf_ptr(name), fn_type);
        assert(LLVMGetIntrinsicID(builtin_fn->fn_val));

        g->memcpy_fn_val = builtin_fn->fn_val;
    }
    {
        BuiltinFnEntry *builtin_fn = create_builtin_fn(g, BuiltinFnIdMemset, "memset");
        builtin_fn->return_type = g->builtin_types.entry_void;
        builtin_fn->param_count = 3;
        builtin_fn->param_types = allocate<TypeTableEntry *>(builtin_fn->param_count);
        builtin_fn->param_types[0] = nullptr; // manually checked later
        builtin_fn->param_types[1] = g->builtin_types.entry_u8;
        builtin_fn->param_types[2] = g->builtin_types.entry_isize;

        LLVMTypeRef param_types[] = {
            LLVMPointerType(LLVMInt8Type(), 0),
            LLVMInt8Type(),
            LLVMIntType(g->pointer_size_bytes * 8),
            LLVMInt32Type(),
            LLVMInt1Type(),
        };
        LLVMTypeRef fn_type = LLVMFunctionType(LLVMVoidType(), param_types, 5, false);
        Buf *name = buf_sprintf("llvm.memset.p0i8.i%d", g->pointer_size_bytes * 8);
        builtin_fn->fn_val = LLVMAddFunction(g->module, buf_ptr(name), fn_type);
        assert(LLVMGetIntrinsicID(builtin_fn->fn_val));

        g->memset_fn_val = builtin_fn->fn_val;
    }
    create_builtin_fn_with_arg_count(g, BuiltinFnIdSizeof, "sizeof", 1);
    create_builtin_fn_with_arg_count(g, BuiltinFnIdMaxValue, "max_value", 1);
    create_builtin_fn_with_arg_count(g, BuiltinFnIdMinValue, "min_value", 1);
    create_builtin_fn_with_arg_count(g, BuiltinFnIdMemberCount, "member_count", 1);
    create_builtin_fn_with_arg_count(g, BuiltinFnIdTypeof, "typeof", 1);
    create_builtin_fn_with_arg_count(g, BuiltinFnIdAddWithOverflow, "add_with_overflow", 4);
    create_builtin_fn_with_arg_count(g, BuiltinFnIdSubWithOverflow, "sub_with_overflow", 4);
    create_builtin_fn_with_arg_count(g, BuiltinFnIdMulWithOverflow, "mul_with_overflow", 4);
    create_builtin_fn_with_arg_count(g, BuiltinFnIdCInclude, "c_include", 1);
    create_builtin_fn_with_arg_count(g, BuiltinFnIdCDefine, "c_define", 2);
    create_builtin_fn_with_arg_count(g, BuiltinFnIdCUndef, "c_undef", 1);
}



static void init(CodeGen *g, Buf *source_path) {
    g->lib_search_paths.append(g->root_source_dir);
    g->lib_search_paths.append(buf_create_from_str(ZIG_STD_DIR));

    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();
    LLVMInitializeAllAsmParsers();
    LLVMInitializeNativeTarget();

    g->is_native_target = true;
    char *native_triple = LLVMGetDefaultTargetTriple();

    g->module = LLVMModuleCreateWithName(buf_ptr(source_path));

    LLVMSetTarget(g->module, native_triple);

    LLVMTargetRef target_ref;
    char *err_msg = nullptr;
    if (LLVMGetTargetFromTriple(native_triple, &target_ref, &err_msg)) {
        zig_panic("unable to get target from triple: %s", err_msg);
    }


    char *native_cpu = LLVMZigGetHostCPUName();
    char *native_features = LLVMZigGetNativeFeatures();

    LLVMCodeGenOptLevel opt_level = (g->build_type == CodeGenBuildTypeDebug) ?
        LLVMCodeGenLevelNone : LLVMCodeGenLevelAggressive;

    LLVMRelocMode reloc_mode = g->is_static ? LLVMRelocStatic : LLVMRelocPIC;

    g->target_machine = LLVMCreateTargetMachine(target_ref, native_triple,
            native_cpu, native_features, opt_level, reloc_mode, LLVMCodeModelDefault);

    g->target_data_ref = LLVMGetTargetMachineData(g->target_machine);

    char *layout_str = LLVMCopyStringRepOfTargetData(g->target_data_ref);
    LLVMSetDataLayout(g->module, layout_str);


    g->pointer_size_bytes = LLVMPointerSize(g->target_data_ref);

    g->builder = LLVMCreateBuilder();
    g->dbuilder = LLVMZigCreateDIBuilder(g->module, true);

    LLVMZigSetFastMath(g->builder, true);


    Buf *producer = buf_sprintf("zig %s", ZIG_VERSION_STRING);
    bool is_optimized = g->build_type == CodeGenBuildTypeRelease;
    const char *flags = "";
    unsigned runtime_version = 0;
    g->compile_unit = LLVMZigCreateCompileUnit(g->dbuilder, LLVMZigLang_DW_LANG_C99(),
            buf_ptr(source_path), buf_ptr(g->root_source_dir),
            buf_ptr(producer), is_optimized, flags, runtime_version,
            "", 0, !g->strip_debug_symbols);

    // This is for debug stuff that doesn't have a real file.
    g->dummy_di_file = nullptr;

    define_builtin_types(g);
    define_builtin_fns(g);

}

static int parse_version_string(Buf *buf, int *major, int *minor, int *patch) {
    char *dot1 = strstr(buf_ptr(buf), ".");
    if (!dot1)
        return ErrorInvalidFormat;
    char *dot2 = strstr(dot1 + 1, ".");
    if (!dot2)
        return ErrorInvalidFormat;

    *major = (int)strtol(buf_ptr(buf), nullptr, 10);
    *minor = (int)strtol(dot1 + 1, nullptr, 10);
    *patch = (int)strtol(dot2 + 1, nullptr, 10);

    return ErrorNone;
}

static void set_root_export_version(CodeGen *g, Buf *version_buf, AstNode *node) {
    int err;
    if ((err = parse_version_string(version_buf, &g->version_major, &g->version_minor, &g->version_patch))) {
        add_node_error(g, node,
                buf_sprintf("invalid version string"));
    }
}


static ImportTableEntry *codegen_add_code(CodeGen *g, Buf *abs_full_path,
        Buf *src_dirname, Buf *src_basename, Buf *source_code)
{
    int err;
    Buf *full_path = buf_alloc();
    os_path_join(src_dirname, src_basename, full_path);

    if (g->verbose) {
        fprintf(stderr, "\nOriginal Source (%s):\n", buf_ptr(full_path));
        fprintf(stderr, "----------------\n");
        fprintf(stderr, "%s\n", buf_ptr(source_code));

        fprintf(stderr, "\nTokens:\n");
        fprintf(stderr, "---------\n");
    }

    Tokenization tokenization = {0};
    tokenize(source_code, &tokenization);

    if (tokenization.err) {
        ErrorMsg *err = err_msg_create_with_line(full_path, tokenization.err_line, tokenization.err_column,
                source_code, tokenization.line_offsets, tokenization.err);

        print_err_msg(err, g->err_color);
        exit(1);
    }

    if (g->verbose) {
        print_tokens(source_code, tokenization.tokens);

        fprintf(stderr, "\nAST:\n");
        fprintf(stderr, "------\n");
    }

    ImportTableEntry *import_entry = allocate<ImportTableEntry>(1);
    import_entry->source_code = source_code;
    import_entry->line_offsets = tokenization.line_offsets;
    import_entry->path = full_path;
    import_entry->fn_table.init(32);
    import_entry->fn_type_table.init(32);

    import_entry->root = ast_parse(source_code, tokenization.tokens, import_entry, g->err_color,
            &g->next_node_index);
    assert(import_entry->root);
    if (g->verbose) {
        ast_print(stderr, import_entry->root, 0);
    }

    import_entry->di_file = LLVMZigCreateFile(g->dbuilder, buf_ptr(src_basename), buf_ptr(src_dirname));
    g->import_table.put(abs_full_path, import_entry);

    import_entry->block_context = new_block_context(import_entry->root, nullptr);
    import_entry->block_context->di_scope = LLVMZigFileToScope(import_entry->di_file);


    assert(import_entry->root->type == NodeTypeRoot);
    for (int decl_i = 0; decl_i < import_entry->root->data.root.top_level_decls.length; decl_i += 1) {
        AstNode *top_level_decl = import_entry->root->data.root.top_level_decls.at(decl_i);

        if (top_level_decl->type == NodeTypeRootExportDecl) {
            if (g->root_import) {
                add_node_error(g, top_level_decl,
                        buf_sprintf("root export declaration only valid in root source file"));
            } else {
                for (int i = 0; i < top_level_decl->data.root_export_decl.directives->length; i += 1) {
                    AstNode *directive_node = top_level_decl->data.root_export_decl.directives->at(i);
                    Buf *name = &directive_node->data.directive.name;
                    Buf *param = &directive_node->data.directive.param;
                    if (buf_eql_str(name, "version")) {
                        set_root_export_version(g, param, directive_node);
                    } else if (buf_eql_str(name, "link")) {
                        g->link_table.put(param, true);
                        if (buf_eql_str(param, "c")) {
                            g->link_libc = true;
                        }
                    } else {
                        add_node_error(g, directive_node,
                                buf_sprintf("invalid directive: '%s'", buf_ptr(name)));
                    }
                }

                if (g->root_export_decl) {
                    add_node_error(g, top_level_decl,
                            buf_sprintf("only one root export declaration allowed"));
                } else {
                    g->root_export_decl = top_level_decl;

                    if (!g->root_out_name)
                        g->root_out_name = &top_level_decl->data.root_export_decl.name;

                    Buf *out_type = &top_level_decl->data.root_export_decl.type;
                    OutType export_out_type;
                    if (buf_eql_str(out_type, "executable")) {
                        export_out_type = OutTypeExe;
                    } else if (buf_eql_str(out_type, "library")) {
                        export_out_type = OutTypeLib;
                    } else if (buf_eql_str(out_type, "object")) {
                        export_out_type = OutTypeObj;
                    } else {
                        add_node_error(g, top_level_decl,
                                buf_sprintf("invalid export type: '%s'", buf_ptr(out_type)));
                    }
                    if (g->out_type == OutTypeUnknown) {
                        g->out_type = export_out_type;
                    }
                }
            }
        } else if (top_level_decl->type == NodeTypeImport) {
            Buf *import_target_path = &top_level_decl->data.import.path;
            Buf full_path = BUF_INIT;
            Buf *import_code = buf_alloc();
            bool found_it = false;

            for (int path_i = 0; path_i < g->lib_search_paths.length; path_i += 1) {
                Buf *search_path = g->lib_search_paths.at(path_i);
                os_path_join(search_path, import_target_path, &full_path);

                Buf *abs_full_path = buf_alloc();
                if ((err = os_path_real(&full_path, abs_full_path))) {
                    if (err == ErrorFileNotFound) {
                        continue;
                    } else {
                        g->error_during_imports = true;
                        add_node_error(g, top_level_decl,
                                buf_sprintf("unable to open '%s': %s", buf_ptr(&full_path), err_str(err)));
                        goto done_looking_at_imports;
                    }
                }

                auto entry = g->import_table.maybe_get(abs_full_path);
                if (entry) {
                    found_it = true;
                    top_level_decl->data.import.import = entry->value;
                } else {
                    if ((err = os_fetch_file_path(abs_full_path, import_code))) {
                        if (err == ErrorFileNotFound) {
                            continue;
                        } else {
                            g->error_during_imports = true;
                            add_node_error(g, top_level_decl,
                                    buf_sprintf("unable to open '%s': %s", buf_ptr(&full_path), err_str(err)));
                            goto done_looking_at_imports;
                        }
                    }
                    top_level_decl->data.import.import = codegen_add_code(g,
                            abs_full_path, search_path, &top_level_decl->data.import.path, import_code);
                    found_it = true;
                }
                break;
            }
            if (!found_it) {
                g->error_during_imports = true;
                add_node_error(g, top_level_decl,
                        buf_sprintf("unable to find '%s'", buf_ptr(import_target_path)));
            }
        } else if (top_level_decl->type == NodeTypeFnDef) {
            AstNode *proto_node = top_level_decl->data.fn_def.fn_proto;
            assert(proto_node->type == NodeTypeFnProto);
            Buf *proto_name = &proto_node->data.fn_proto.name;

            bool is_private = (proto_node->data.fn_proto.visib_mod == VisibModPrivate);

            if (buf_eql_str(proto_name, "main") && !is_private) {
                g->have_exported_main = true;
            }
        }
    }

done_looking_at_imports:

    return import_entry;
}

static ImportTableEntry *add_special_code(CodeGen *g, const char *basename) {
    Buf *std_dir = buf_create_from_str(ZIG_STD_DIR);
    Buf *code_basename = buf_create_from_str(basename);
    Buf path_to_code_src = BUF_INIT;
    os_path_join(std_dir, code_basename, &path_to_code_src);
    Buf *abs_full_path = buf_alloc();
    int err;
    if ((err = os_path_real(&path_to_code_src, abs_full_path))) {
        zig_panic("unable to open '%s': %s", buf_ptr(&path_to_code_src), err_str(err));
    }
    Buf *import_code = buf_alloc();
    if ((err = os_fetch_file_path(abs_full_path, import_code))) {
        zig_panic("unable to open '%s': %s", buf_ptr(&path_to_code_src), err_str(err));
    }

    return codegen_add_code(g, abs_full_path, std_dir, code_basename, import_code);
}

void codegen_add_root_code(CodeGen *g, Buf *src_dir, Buf *src_basename, Buf *source_code) {
    Buf source_path = BUF_INIT;
    os_path_join(src_dir, src_basename, &source_path);
    init(g, &source_path);

    Buf *abs_full_path = buf_alloc();
    int err;
    if ((err = os_path_real(&source_path, abs_full_path))) {
        zig_panic("unable to open '%s': %s", buf_ptr(&source_path), err_str(err));
    }

    g->root_import = codegen_add_code(g, abs_full_path, src_dir, src_basename, source_code);

    if (!g->root_out_name) {
        add_node_error(g, g->root_import->root,
                buf_sprintf("missing export declaration and output name not provided"));
    } else if (g->out_type == OutTypeUnknown) {
        add_node_error(g, g->root_import->root,
                buf_sprintf("missing export declaration and export type not provided"));
    }

    if (!g->link_libc) {
        if (g->have_exported_main && (g->out_type == OutTypeObj || g->out_type == OutTypeExe)) {
            g->bootstrap_import = add_special_code(g, "bootstrap.zig");
        }

        if (g->out_type == OutTypeExe) {
            add_special_code(g, "builtin.zig");
        }
    }

    if (g->verbose) {
        fprintf(stderr, "\nSemantic Analysis:\n");
        fprintf(stderr, "--------------------\n");
    }
    if (!g->error_during_imports) {
        semantic_analyze(g);
    }

    if (g->errors.length == 0) {
        if (g->verbose) {
            fprintf(stderr, "OK\n");
        }
    } else {
        for (int i = 0; i < g->errors.length; i += 1) {
            ErrorMsg *err = g->errors.at(i);
            print_err_msg(err, g->err_color);
        }
        exit(1);
    }

    if (g->verbose) {
        fprintf(stderr, "\nCode Generation:\n");
        fprintf(stderr, "------------------\n");
    }

    do_code_gen(g);
}

static void to_c_type(CodeGen *g, AstNode *type_node, Buf *out_buf) {
    zig_panic("TODO this function needs some love");
    TypeTableEntry *type_entry = get_resolved_expr(type_node)->type_entry;
    assert(type_entry);

    if (type_entry == g->builtin_types.entry_u8) {
        g->c_stdint_used = true;
        buf_init_from_str(out_buf, "uint8_t");
    } else if (type_entry == g->builtin_types.entry_i32) {
        g->c_stdint_used = true;
        buf_init_from_str(out_buf, "int32_t");
    } else if (type_entry == g->builtin_types.entry_isize) {
        g->c_stdint_used = true;
        buf_init_from_str(out_buf, "intptr_t");
    } else if (type_entry == g->builtin_types.entry_f32) {
        buf_init_from_str(out_buf, "float");
    } else if (type_entry == g->builtin_types.entry_unreachable) {
        buf_init_from_str(out_buf, "__attribute__((__noreturn__)) void");
    } else if (type_entry == g->builtin_types.entry_bool) {
        buf_init_from_str(out_buf, "unsigned char");
    } else if (type_entry == g->builtin_types.entry_void) {
        buf_init_from_str(out_buf, "void");
    } else {
        zig_panic("TODO to_c_type");
    }
}

static void generate_h_file(CodeGen *g) {
    Buf *h_file_out_path = buf_sprintf("%s.h", buf_ptr(g->root_out_name));
    FILE *out_h = fopen(buf_ptr(h_file_out_path), "wb");
    if (!out_h)
        zig_panic("unable to open %s: %s", buf_ptr(h_file_out_path), strerror(errno));

    Buf *export_macro = buf_sprintf("%s_EXPORT", buf_ptr(g->root_out_name));
    buf_upcase(export_macro);

    Buf *extern_c_macro = buf_sprintf("%s_EXTERN_C", buf_ptr(g->root_out_name));
    buf_upcase(extern_c_macro);

    Buf h_buf = BUF_INIT;
    buf_resize(&h_buf, 0);
    for (int fn_def_i = 0; fn_def_i < g->fn_defs.length; fn_def_i += 1) {
        FnTableEntry *fn_table_entry = g->fn_defs.at(fn_def_i);
        AstNode *proto_node = fn_table_entry->proto_node;
        assert(proto_node->type == NodeTypeFnProto);
        AstNodeFnProto *fn_proto = &proto_node->data.fn_proto;

        if (fn_proto->visib_mod != VisibModExport)
            continue;

        Buf return_type_c = BUF_INIT;
        to_c_type(g, fn_proto->return_type, &return_type_c);

        buf_appendf(&h_buf, "%s %s %s(",
                buf_ptr(export_macro),
                buf_ptr(&return_type_c),
                buf_ptr(&fn_proto->name));

        Buf param_type_c = BUF_INIT;
        if (fn_proto->params.length) {
            for (int param_i = 0; param_i < fn_proto->params.length; param_i += 1) {
                AstNode *param_decl_node = fn_proto->params.at(param_i);
                AstNode *param_type = param_decl_node->data.param_decl.type;
                to_c_type(g, param_type, &param_type_c);
                buf_appendf(&h_buf, "%s %s",
                        buf_ptr(&param_type_c),
                        buf_ptr(&param_decl_node->data.param_decl.name));
                if (param_i < fn_proto->params.length - 1)
                    buf_appendf(&h_buf, ", ");
            }
            buf_appendf(&h_buf, ")");
        } else {
            buf_appendf(&h_buf, "void)");
        }

        buf_appendf(&h_buf, ";\n");

    }

    Buf *ifdef_dance_name = buf_sprintf("%s_%s_H",
            buf_ptr(g->root_out_name), buf_ptr(g->root_out_name));
    buf_upcase(ifdef_dance_name);

    fprintf(out_h, "#ifndef %s\n", buf_ptr(ifdef_dance_name));
    fprintf(out_h, "#define %s\n\n", buf_ptr(ifdef_dance_name));

    if (g->c_stdint_used)
        fprintf(out_h, "#include <stdint.h>\n");

    fprintf(out_h, "\n");

    fprintf(out_h, "#ifdef __cplusplus\n");
    fprintf(out_h, "#define %s extern \"C\"\n", buf_ptr(extern_c_macro));
    fprintf(out_h, "#else\n");
    fprintf(out_h, "#define %s\n", buf_ptr(extern_c_macro));
    fprintf(out_h, "#endif\n");
    fprintf(out_h, "\n");
    fprintf(out_h, "#if defined(_WIN32)\n");
    fprintf(out_h, "#define %s %s __declspec(dllimport)\n", buf_ptr(export_macro), buf_ptr(extern_c_macro));
    fprintf(out_h, "#else\n");
    fprintf(out_h, "#define %s %s __attribute__((visibility (\"default\")))\n",
            buf_ptr(export_macro), buf_ptr(extern_c_macro));
    fprintf(out_h, "#endif\n");
    fprintf(out_h, "\n");

    fprintf(out_h, "%s", buf_ptr(&h_buf));

    fprintf(out_h, "\n#endif\n");

    if (fclose(out_h))
        zig_panic("unable to close h file: %s", strerror(errno));
}

static const char *get_libc_file(CodeGen *g, const char *file) {
    Buf *out_buf = buf_alloc();
    os_path_join(g->libc_lib_path, buf_create_from_str(file), out_buf);
    return buf_ptr(out_buf);
}

void codegen_link(CodeGen *g, const char *out_file) {
    bool is_optimized = (g->build_type == CodeGenBuildTypeRelease);
    if (is_optimized) {
        if (g->verbose) {
            fprintf(stderr, "\nOptimization:\n");
            fprintf(stderr, "---------------\n");
        }

        LLVMZigOptimizeModule(g->target_machine, g->module);

        if (g->verbose) {
            LLVMDumpModule(g->module);
        }
    }
    if (g->verbose) {
        fprintf(stderr, "\nLink:\n");
        fprintf(stderr, "-------\n");
    }

    if (!out_file) {
        out_file = buf_ptr(g->root_out_name);
    }

    Buf out_file_o = BUF_INIT;
    buf_init_from_str(&out_file_o, out_file);

    if (g->out_type != OutTypeObj) {
        buf_append_str(&out_file_o, ".o");
    }

    char *err_msg = nullptr;
    if (LLVMTargetMachineEmitToFile(g->target_machine, g->module, buf_ptr(&out_file_o),
                LLVMObjectFile, &err_msg))
    {
        zig_panic("unable to write object file: %s", err_msg);
    }

    if (g->out_type == OutTypeObj) {
        if (g->verbose) {
            fprintf(stderr, "OK\n");
        }
        return;
    }

    if (g->out_type == OutTypeLib && g->is_static) {
        // invoke `ar`
        // example:
        // # static link into libfoo.a
        // ar rcs libfoo.a foo1.o foo2.o
        zig_panic("TODO invoke ar");
        return;
    }

    // invoke `ld`
    ZigList<const char *> args = {0};
    const char *crt1o;
    if (g->is_static) {
        args.append("-static");
        crt1o = "crt1.o";
    } else {
        crt1o = "Scrt1.o";
    }

    // TODO don't pass this parameter unless linking with libc
    char *ZIG_NATIVE_DYNAMIC_LINKER = getenv("ZIG_NATIVE_DYNAMIC_LINKER");
    if (g->is_native_target && ZIG_NATIVE_DYNAMIC_LINKER) {
        if (ZIG_NATIVE_DYNAMIC_LINKER[0] != 0) {
            args.append("-dynamic-linker");
            args.append(ZIG_NATIVE_DYNAMIC_LINKER);
        }
    } else {
        args.append("-dynamic-linker");
        args.append(buf_ptr(get_dynamic_linker(g->target_machine)));
    }

    if (g->out_type == OutTypeLib) {
        Buf *out_lib_so = buf_sprintf("lib%s.so.%d.%d.%d",
                buf_ptr(g->root_out_name), g->version_major, g->version_minor, g->version_patch);
        Buf *soname = buf_sprintf("lib%s.so.%d", buf_ptr(g->root_out_name), g->version_major);
        args.append("-shared");
        args.append("-soname");
        args.append(buf_ptr(soname));
        out_file = buf_ptr(out_lib_so);
    }

    args.append("-o");
    args.append(out_file);

    bool link_in_crt = (g->link_libc && g->out_type == OutTypeExe);

    if (link_in_crt) {
        find_libc_path(g);

        args.append(get_libc_file(g, crt1o));
        args.append(get_libc_file(g, "crti.o"));
    }

    args.append((const char *)buf_ptr(&out_file_o));

    if (link_in_crt) {
        args.append(get_libc_file(g, "crtn.o"));
    }

    auto it = g->link_table.entry_iterator();
    for (;;) {
        auto *entry = it.next();
        if (!entry)
            break;

        Buf *arg = buf_sprintf("-l%s", buf_ptr(entry->key));
        args.append(buf_ptr(arg));
    }

    if (g->verbose) {
        fprintf(stderr, "ld");
        for (int i = 0; i < args.length; i += 1) {
            fprintf(stderr, " %s", args.at(i));
        }
        fprintf(stderr, "\n");
    }

    int return_code;
    Buf ld_stderr = BUF_INIT;
    Buf ld_stdout = BUF_INIT;
    os_exec_process("ld", args, &return_code, &ld_stderr, &ld_stdout);

    if (return_code != 0) {
        fprintf(stderr, "ld failed with return code %d\n", return_code);
        fprintf(stderr, "%s\n", buf_ptr(&ld_stderr));
        exit(1);
    } else if (buf_len(&ld_stderr)) {
        fprintf(stderr, "%s\n", buf_ptr(&ld_stderr));
    }

    if (g->out_type == OutTypeLib) {
        generate_h_file(g);
    }

    if (g->verbose) {
        fprintf(stderr, "OK\n");
    }
}
