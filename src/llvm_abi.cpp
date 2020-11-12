enum lbArgKind {
	lbArg_Direct,
	lbArg_Indirect,
	lbArg_Ignore,
};

struct lbArgType {
	lbArgKind kind;
	LLVMTypeRef type;
	LLVMTypeRef cast_type;      // Optional
	LLVMTypeRef pad_type;       // Optional
	LLVMAttributeRef attribute; // Optional
};

lbArgType lb_arg_type_direct(LLVMTypeRef type, LLVMTypeRef cast_type, LLVMTypeRef pad_type, LLVMAttributeRef attr) {
	return lbArgType{lbArg_Direct, type, cast_type, pad_type, attr};
}
lbArgType lb_arg_type_direct(LLVMTypeRef type) {
	return lb_arg_type_direct(type, nullptr, nullptr, nullptr);
}

lbArgType lb_arg_type_indirect(LLVMTypeRef type, LLVMAttributeRef attr) {
	return lbArgType{lbArg_Indirect, type, nullptr, nullptr, attr};
}

lbArgType lb_arg_type_ignore(LLVMTypeRef type) {
	return lbArgType{lbArg_Ignore, type, nullptr, nullptr, nullptr};
}

struct lbFunctionType {
	LLVMContextRef   ctx;
	ProcCallingConvention calling_convention;
	Array<lbArgType> args;
	lbArgType        ret;
};


bool lb_is_type_kind(LLVMTypeRef type, LLVMTypeKind kind) {
	return LLVMGetTypeKind(type) == kind;
}

LLVMTypeRef lb_function_type_to_llvm_ptr(lbFunctionType *ft, bool is_var_arg) {
	unsigned arg_count = cast(unsigned)ft->args.count;
	unsigned offset = 0;

	LLVMTypeRef ret = nullptr;
	if (ft->ret.kind == lbArg_Direct) {
		if (ft->ret.cast_type != nullptr) {
			ret = ft->ret.cast_type;
		} else {
			ret = ft->ret.type;
		}
	} else if (ft->ret.kind == lbArg_Indirect) {
		offset += 1;
		ret = LLVMVoidTypeInContext(ft->ctx);
	} else if (ft->ret.kind == lbArg_Ignore) {
		ret = LLVMVoidTypeInContext(ft->ctx);
	}
	GB_ASSERT_MSG(ret != nullptr, "%d", ft->ret.kind);

	unsigned maximum_arg_count = offset+arg_count;
	LLVMTypeRef *args = gb_alloc_array(heap_allocator(), LLVMTypeRef, maximum_arg_count);
	if (offset == 1) {
		GB_ASSERT(ft->ret.kind == lbArg_Indirect);
		args[0] = ft->ret.type;
	}

	unsigned arg_index = offset;
	for (unsigned i = 0; i < arg_count; i++) {
		lbArgType *arg = &ft->args[i];
		if (arg->kind == lbArg_Direct) {
			LLVMTypeRef arg_type = nullptr;
			if (ft->args[i].cast_type != nullptr) {
				arg_type = arg->cast_type;
			} else {
				arg_type = arg->type;
			}
			args[arg_index++] = arg_type;
		} else if (arg->kind == lbArg_Indirect) {
			GB_ASSERT(!lb_is_type_kind(arg->type, LLVMPointerTypeKind));
			args[arg_index++] = LLVMPointerType(arg->type, 0);
		} else if (arg->kind == lbArg_Ignore) {
			// ignore
		}
	}
	unsigned total_arg_count = arg_index;
	LLVMTypeRef func_type = LLVMFunctionType(ret, args, total_arg_count, is_var_arg);
	return LLVMPointerType(func_type, 0);
}


void lb_add_function_type_attributes(LLVMValueRef fn, lbFunctionType *ft, ProcCallingConvention calling_convention) {
	if (ft == nullptr) {
		return;
	}
	unsigned arg_count = cast(unsigned)ft->args.count;
	unsigned offset = 0;
	if (ft->ret.kind == lbArg_Indirect) {
		offset += 1;
	}

	unsigned arg_index = offset;
	for (unsigned i = 0; i < arg_count; i++) {
		lbArgType *arg = &ft->args[i];
		if (arg->kind == lbArg_Ignore) {
			continue;
		}

		if (arg->attribute) {
			LLVMAddAttributeAtIndex(fn, arg_index+1, arg->attribute);
		}

		arg_index++;
	}

	if (offset != 0 && ft->ret.kind == lbArg_Indirect && ft->ret.attribute != nullptr) {
		LLVMAddAttributeAtIndex(fn, offset, ft->ret.attribute);
	}

	lbCallingConventionKind cc_kind = lbCallingConvention_C;
	// TODO(bill): Clean up this logic
	if (build_context.metrics.os != TargetOs_js)  {
		cc_kind = lb_calling_convention_map[calling_convention];
	}
	LLVMSetFunctionCallConv(fn, cc_kind);
	if (calling_convention == ProcCC_Odin) {
		unsigned context_index = offset+arg_count;
		LLVMContextRef c = ft->ctx;
		LLVMAddAttributeAtIndex(fn, context_index, lb_create_enum_attribute(c, "noalias", true));
		LLVMAddAttributeAtIndex(fn, context_index, lb_create_enum_attribute(c, "nonnull", true));
		LLVMAddAttributeAtIndex(fn, context_index, lb_create_enum_attribute(c, "nocapture", true));
	}

}

i64 lb_sizeof(LLVMTypeRef type);
i64 lb_alignof(LLVMTypeRef type);

i64 lb_sizeof(LLVMTypeRef type) {
	LLVMTypeKind kind = LLVMGetTypeKind(type);
	switch (kind) {
	case LLVMVoidTypeKind:
		return 0;
	case LLVMIntegerTypeKind:
		{
			unsigned w = LLVMGetIntTypeWidth(type);
			return (w + 7)/8;
		}
	case LLVMFloatTypeKind:
		return 4;
	case LLVMDoubleTypeKind:
		return 8;
	case LLVMPointerTypeKind:
		return build_context.word_size;
	case LLVMStructTypeKind:
		{
			unsigned field_count = LLVMCountStructElementTypes(type);
			i64 offset = 0;
			if (LLVMIsPackedStruct(type)) {
				for (unsigned i = 0; i < field_count; i++) {
					LLVMTypeRef field = LLVMStructGetTypeAtIndex(type, i);
					offset += lb_sizeof(field);
				}
			} else {
				for (unsigned i = 0; i < field_count; i++) {
					LLVMTypeRef field = LLVMStructGetTypeAtIndex(type, i);
					i64 align = lb_alignof(field);
					offset = align_formula(offset, align);
					offset += lb_sizeof(field);
				}
			}
			offset = align_formula(offset, lb_alignof(type));
			return offset;
		}
		break;
	case LLVMArrayTypeKind:
		{
			LLVMTypeRef elem = LLVMGetElementType(type);
			i64 elem_size = lb_sizeof(elem);
			i64 count = LLVMGetVectorSize(type);
			i64 size = count * elem_size;
			return size;
		}
		break;

	case LLVMX86_MMXTypeKind:
		return 8;
	case LLVMVectorTypeKind:
		{
			LLVMTypeRef elem = LLVMGetElementType(type);
			i64 elem_size = lb_sizeof(elem);
			i64 count = LLVMGetVectorSize(type);
			i64 size = count * elem_size;
			return gb_clamp(next_pow2(size), 1, build_context.max_align);
		}

	}
	GB_PANIC("Unhandled type for lb_sizeof -> %s", LLVMPrintTypeToString(type));

	// LLVMValueRef v = LLVMSizeOf(type);
	// GB_ASSERT(LLVMIsConstant(v));
	// return cast(i64)LLVMConstIntGetSExtValue(v);
	return 0;
}

i64 lb_alignof(LLVMTypeRef type) {
	LLVMTypeKind kind = LLVMGetTypeKind(type);
	switch (kind) {
	case LLVMVoidTypeKind:
		return 1;
	case LLVMIntegerTypeKind:
		{
			unsigned w = LLVMGetIntTypeWidth(type);
			return gb_clamp((w + 7)/8, 1, build_context.max_align);
		}
	case LLVMFloatTypeKind:
		return 4;
	case LLVMDoubleTypeKind:
		return 8;
	case LLVMPointerTypeKind:
		return build_context.word_size;
	case LLVMStructTypeKind:
		{
			if (LLVMIsPackedStruct(type)) {
				return 1;
			} else {
				unsigned field_count = LLVMCountStructElementTypes(type);
				i64 max_align = 1;
				for (unsigned i = 0; i < field_count; i++) {
					LLVMTypeRef field = LLVMStructGetTypeAtIndex(type, i);
					i64 field_align = lb_alignof(field);
					max_align = gb_max(max_align, field_align);
				}
				return max_align;
			}
		}
		break;
	case LLVMArrayTypeKind:
		{
			LLVMTypeRef elem = LLVMGetElementType(type);
			i64 elem_size = lb_sizeof(elem);
			i64 count = LLVMGetVectorSize(type);
			i64 size = count * elem_size;
			return size;
		}
		break;

	case LLVMX86_MMXTypeKind:
		return 8;
	case LLVMVectorTypeKind:
		{
			LLVMTypeRef elem = LLVMGetElementType(type);
			i64 elem_size = lb_sizeof(elem);
			i64 count = LLVMGetVectorSize(type);
			i64 size = count * elem_size;
			return gb_clamp(next_pow2(size), 1, build_context.max_align);
		}

	}
	GB_PANIC("Unhandled type for lb_sizeof -> %s", LLVMPrintTypeToString(type));

	// LLVMValueRef v = LLVMAlignOf(type);
	// GB_ASSERT(LLVMIsConstant(v));
	// return LLVMConstIntGetSExtValue(v);
	return 1;
}

Type *lb_abi_to_odin_type(LLVMTypeRef type) {
	LLVMTypeKind kind = LLVMGetTypeKind(type);
	switch (kind) {
	case LLVMVoidTypeKind:
		return nullptr;
	case LLVMIntegerTypeKind:
		{
			unsigned w = LLVMGetIntTypeWidth(type);
			if (w == 1) {
				return t_llvm_bool;
			}
			unsigned bytes = (w + 7)/8;
			switch (bytes) {
			case 1: return t_u8;
			case 2: return t_u16;
			case 4: return t_u32;
			case 8: return t_u64;
			case 16: return t_u128;
			}
			GB_PANIC("Unhandled integer type");
		}
	case LLVMFloatTypeKind:
		return t_f32;
	case LLVMDoubleTypeKind:
		return t_f64;
	case LLVMPointerTypeKind:
		return t_rawptr;
	case LLVMStructTypeKind:
		{
			GB_PANIC("HERE");
		}
		break;
	case LLVMArrayTypeKind:
		{

			i64 count = LLVMGetVectorSize(type);
			Type *elem = lb_abi_to_odin_type(LLVMGetElementType(type));
			return alloc_type_array(elem, count);
		}
		break;

	case LLVMX86_MMXTypeKind:
		return t_vector_x86_mmx;
	case LLVMVectorTypeKind:
		{
			i64 count = LLVMGetVectorSize(type);
			Type *elem = lb_abi_to_odin_type(LLVMGetElementType(type));
			return alloc_type_simd_vector(count, elem);
		}

	}
	GB_PANIC("Unhandled type for lb_abi_to_odin_type -> %s", LLVMPrintTypeToString(type));

	// LLVMValueRef v = LLVMSizeOf(type);
	// GB_ASSERT(LLVMIsConstant(v));
	// return cast(i64)LLVMConstIntGetSExtValue(v);
	return 0;
}



#define LB_ABI_INFO(name) lbFunctionType *name(LLVMContextRef c, LLVMTypeRef *arg_types, unsigned arg_count, LLVMTypeRef return_type, bool return_is_defined, ProcCallingConvention calling_convention)
typedef LB_ABI_INFO(lbAbiInfoType);


// NOTE(bill): I hate `namespace` in C++ but this is just because I don't want to prefix everything
namespace lbAbi386 {
	Array<lbArgType> compute_arg_types(LLVMContextRef c, LLVMTypeRef *arg_types, unsigned arg_count);
	lbArgType compute_return_type(LLVMContextRef c, LLVMTypeRef return_type, bool return_is_defined);

	LB_ABI_INFO(abi_info) {
		lbFunctionType *ft = gb_alloc_item(heap_allocator(), lbFunctionType);
		ft->ctx = c;
		ft->args = compute_arg_types(c, arg_types, arg_count);
		ft->ret = compute_return_type(c, return_type, return_is_defined);
		ft->calling_convention = calling_convention;
		return ft;
	}

	lbArgType non_struct(LLVMContextRef c, LLVMTypeRef type) {
		if (build_context.metrics.os == TargetOs_windows &&
		           build_context.word_size == 8 &&
		           lb_is_type_kind(type, LLVMIntegerTypeKind) &&
		           lb_sizeof(type) == 16) {

			LLVMTypeRef cast_type = LLVMVectorType(LLVMInt64TypeInContext(c), 2);
			return lb_arg_type_direct(type, cast_type, nullptr, nullptr);
		}



		LLVMAttributeRef attr = nullptr;
		LLVMTypeRef i1 = LLVMInt1TypeInContext(c);
		if (type == i1) {
			// attr = lb_create_enum_attribute(c, "zext", true);
			// return lb_arg_type_direct(type, i1, nullptr, attr);
		}
		return lb_arg_type_direct(type, nullptr, nullptr, attr);
	}

	Array<lbArgType> compute_arg_types(LLVMContextRef c, LLVMTypeRef *arg_types, unsigned arg_count) {
		auto args = array_make<lbArgType>(heap_allocator(), arg_count);

		for (unsigned i = 0; i < arg_count; i++) {
			LLVMTypeRef t = arg_types[i];
			LLVMTypeKind kind = LLVMGetTypeKind(t);
			if (kind == LLVMStructTypeKind) {
				i64 sz = lb_sizeof(t);
				if (sz == 0) {
					args[i] = lb_arg_type_ignore(t);
				} else {
					args[i] = lb_arg_type_indirect(t, lb_create_enum_attribute(c, "byval", true));
				}
			} else {
				args[i] = non_struct(c, t);
			}
		}
		return args;
	}

	lbArgType compute_return_type(LLVMContextRef c, LLVMTypeRef return_type, bool return_is_defined) {
		if (!return_is_defined) {
			return lb_arg_type_direct(LLVMVoidTypeInContext(c));
		} else if (lb_is_type_kind(return_type, LLVMStructTypeKind) || lb_is_type_kind(return_type, LLVMArrayTypeKind)) {
			i64 sz = lb_sizeof(return_type);
			switch (sz) {
			case 1: return lb_arg_type_direct(return_type, LLVMIntTypeInContext(c,  8), nullptr, nullptr);
			case 2: return lb_arg_type_direct(return_type, LLVMIntTypeInContext(c, 16), nullptr, nullptr);
			case 4: return lb_arg_type_direct(return_type, LLVMIntTypeInContext(c, 32), nullptr, nullptr);
			case 8: return lb_arg_type_direct(return_type, LLVMIntTypeInContext(c, 64), nullptr, nullptr);
			}
			return lb_arg_type_indirect(LLVMPointerType(return_type, 0), lb_create_enum_attribute(c, "sret", true));
		}
		return non_struct(c, return_type);
	}
};

namespace lbAbiAmd64Win64 {
	Array<lbArgType> compute_arg_types(LLVMContextRef c, LLVMTypeRef *arg_types, unsigned arg_count);


	LB_ABI_INFO(abi_info) {
		lbFunctionType *ft = gb_alloc_item(heap_allocator(), lbFunctionType);
		ft->ctx = c;
		ft->args = compute_arg_types(c, arg_types, arg_count);
		ft->ret = lbAbi386::compute_return_type(c, return_type, return_is_defined);
		ft->calling_convention = calling_convention;
		return ft;
	}

	Array<lbArgType> compute_arg_types(LLVMContextRef c, LLVMTypeRef *arg_types, unsigned arg_count) {
		auto args = array_make<lbArgType>(heap_allocator(), arg_count);

		for (unsigned i = 0; i < arg_count; i++) {
			LLVMTypeRef t = arg_types[i];
			LLVMTypeKind kind = LLVMGetTypeKind(t);
			if (kind == LLVMStructTypeKind) {
				i64 sz = lb_sizeof(t);
				switch (sz) {
				case 1:
				case 2:
				case 4:
				case 8:
					args[i] = lb_arg_type_direct(t, LLVMIntTypeInContext(c, 8*cast(unsigned)sz), nullptr, nullptr);
					break;
				default:
					args[i] = lb_arg_type_indirect(t, nullptr);
					break;
				}
			} else {
				args[i] = lbAbi386::non_struct(c, t);
			}
		}
		return args;
	}
};

// NOTE(bill): I hate `namespace` in C++ but this is just because I don't want to prefix everything
namespace lbAbiAmd64SysV {
	enum RegClass {
		RegClass_NoClass,
		RegClass_Int,
		RegClass_SSEFs,
		RegClass_SSEFv,
		RegClass_SSEDs,
		RegClass_SSEDv,
		RegClass_SSEInt,
		RegClass_SSEUp,
		RegClass_X87,
		RegClass_X87Up,
		RegClass_ComplexX87,
		RegClass_Memory,
	};

	bool is_sse(RegClass reg_class) {
		switch (reg_class) {
		case RegClass_SSEFs:
		case RegClass_SSEFv:
		case RegClass_SSEDv:
			return true;
		}
		return false;
	}

	void all_mem(Array<RegClass> *cs) {
		for_array(i, *cs) {
			(*cs)[i] = RegClass_Memory;
		}
	}

	Array<lbArgType> compute_arg_types(LLVMContextRef c, LLVMTypeRef *arg_types, unsigned arg_count);
	lbArgType compute_return_type(LLVMContextRef c, LLVMTypeRef return_type, bool return_is_defined);
	void classify_with(LLVMTypeRef t, Array<RegClass> *cls, i64 ix, i64 off);
	void fixup(LLVMTypeRef t, Array<RegClass> *cls);

	LB_ABI_INFO(abi_info) {
		lbFunctionType *ft = gb_alloc_item(heap_allocator(), lbFunctionType);
		ft->ctx = c;
		// TODO(bill): THIS IS VERY VERY WRONG!
		ft->args = compute_arg_types(c, arg_types, arg_count);
		ft->ret = compute_return_type(c, return_type, return_is_defined);
		ft->calling_convention = calling_convention;
		return ft;
	}

	lbArgType non_struct(LLVMContextRef c, LLVMTypeRef type) {
		LLVMAttributeRef attr = nullptr;
		LLVMTypeRef i1 = LLVMInt1TypeInContext(c);
		if (type == i1) {
			attr = lb_create_enum_attribute(c, "zext", true);
		}
		return lb_arg_type_direct(type, nullptr, nullptr, attr);
	}

	Array<RegClass> classify(LLVMTypeRef t) {
		i64 sz = lb_sizeof(t);
		i64 words = (sz + 7)/8;
		auto reg_classes = array_make<RegClass>(heap_allocator(), cast(isize)words);
		if (words > 4) {
			all_mem(&reg_classes);
		} else {
			classify_with(t, &reg_classes, 0, 0);
			fixup(t, &reg_classes);
		}
		return reg_classes;
	}

	void classify_struct(LLVMTypeRef *fields, unsigned field_count, Array<RegClass> *cls, i64 i, i64 off, LLVMBool packed) {
		i64 field_off = off;
		for (unsigned i = 0; i < field_count; i++) {
			LLVMTypeRef t = fields[i];
			if (!packed) {
				field_off = align_formula(field_off, lb_alignof(t));
			}
			classify_with(t, cls, i, field_off);
			field_off += lb_sizeof(t);
		}
	}

	void unify(Array<RegClass> *cls, i64 i, RegClass newv) {
		RegClass &oldv = (*cls)[i];
		if (oldv == newv) {
			return;
		} else if (oldv == RegClass_NoClass) {
			oldv = newv;
		} else if (newv == RegClass_NoClass) {
			return;
		} else if (oldv == RegClass_Memory || newv == RegClass_Memory) {
			return;
		} else if (oldv == RegClass_Int || newv	== RegClass_Int) {
			return;
		} else if (oldv == RegClass_X87 || oldv == RegClass_X87Up || oldv == RegClass_ComplexX87 ||
		           newv == RegClass_X87 || newv == RegClass_X87Up || newv == RegClass_ComplexX87) {
			oldv = RegClass_Memory;
		} else {
			oldv = newv;
		}
	}

	void fixup(LLVMTypeRef t, Array<RegClass> *cls) {
		i64 i = 0;
		i64 e = cls->count;
		if (e > 2 && (lb_is_type_kind(t, LLVMStructTypeKind) || lb_is_type_kind(t, LLVMArrayTypeKind))) {
			RegClass &oldv = (*cls)[i];
			if (is_sse(oldv)) {
				for (i++; i < e; i++) {
					if (oldv != RegClass_SSEUp) {
						all_mem(cls);
						return;
					}
				}
			} else {
				all_mem(cls);
				return;
			}
		} else {
			while (i < e) {
				RegClass &oldv = (*cls)[i];
				if (oldv == RegClass_Memory) {
					all_mem(cls);
					return;
				} else if (oldv == RegClass_X87Up) {
					// NOTE(bill): Darwin
					all_mem(cls);
					return;
				} else if (oldv == RegClass_SSEUp) {
					oldv = RegClass_SSEDv;
				} else if (is_sse(oldv)) {
					i++;
					while (i != e && oldv == RegClass_SSEUp) {
						i++;
					}
				} else if (oldv == RegClass_X87) {
					i++;
					while (i != e && oldv == RegClass_X87Up) {
						i++;
					}
				} else {
					i++;
				}
			}
		}
	}

	unsigned llvec_len(Array<RegClass> const &reg_classes) {
		unsigned len = 1;
		for_array(i, reg_classes) {
			if (reg_classes[i] != RegClass_SSEUp) {
				break;
			}
			len++;
		}
		return len;
	}


	LLVMTypeRef llreg(LLVMContextRef c, Array<RegClass> const &reg_classes) {;
		auto types = array_make<LLVMTypeRef>(heap_allocator(), 0, reg_classes.count);
		for_array(i, reg_classes) {
			switch (reg_classes[i]) {
			case RegClass_Int:
				array_add(&types, LLVMIntTypeInContext(c, 64));
				break;
			case RegClass_SSEFv:
				{
					unsigned vec_len = llvec_len(array_slice(reg_classes, i+1, reg_classes.count));
					LLVMTypeRef vec_type = LLVMVectorType(LLVMFloatTypeInContext(c), vec_len);
					array_add(&types, vec_type);
					i += vec_len;
					continue;
				}
				break;
			case RegClass_SSEFs:
				array_add(&types, LLVMFloatTypeInContext(c));
				break;
			case RegClass_SSEDs:
				array_add(&types, LLVMDoubleTypeInContext(c));
				break;
			default:
				GB_PANIC("Unhandled RegClass");
			}
		}

		return LLVMStructTypeInContext(c, types.data, cast(unsigned)types.count, false);
	}

	void classify_with(LLVMTypeRef t, Array<RegClass> *cls, i64 ix, i64 off) {
		i64 t_align = lb_alignof(t);
		i64 t_size  = lb_sizeof(t);

		i64 mis_align = off % t_align;
		if (mis_align != 0) {
			i64 e = (off + t_size + 7) / 8;
			for (i64 i = off / 8; i < e; i++) {
				unify(cls, ix+1, RegClass_Memory);
			}
			return;
		}

		switch (LLVMGetTypeKind(t)) {
		case LLVMIntegerTypeKind:
		case LLVMPointerTypeKind:
			unify(cls, ix+off / 8, RegClass_Int);
			break;
		case LLVMFloatTypeKind:
			unify(cls, ix+off / 8, (off%8 == 4) ? RegClass_SSEFv : RegClass_SSEFs);
			break;
		case LLVMDoubleTypeKind:
			unify(cls, ix+off / 8,  RegClass_SSEDs);
			break;
		case LLVMStructTypeKind:
			{
				unsigned field_count = LLVMCountStructElementTypes(t);
				LLVMTypeRef *fields = gb_alloc_array(heap_allocator(), LLVMTypeRef, field_count); // HACK(bill): LEAK
				defer (gb_free(heap_allocator(), fields));

				LLVMGetStructElementTypes(t, fields);

				classify_struct(fields, field_count, cls, ix, off, LLVMIsPackedStruct(t));
			}
			break;
		case LLVMArrayTypeKind:
			{
				i64 len = LLVMGetArrayLength(t);
				LLVMTypeRef elem = LLVMGetElementType(t);
				i64 elem_sz = lb_sizeof(elem);
				for (i64 i = 0; i < len; i++) {
					classify_with(elem, cls, ix, off + i*elem_sz);
				}
			}
			break;
		default:
			GB_PANIC("Unhandled type");
			break;
		}
	}

	Array<lbArgType> compute_arg_types(LLVMContextRef c, LLVMTypeRef *arg_types, unsigned arg_count) {
		auto args = array_make<lbArgType>(heap_allocator(), arg_count);

		for (unsigned i = 0; i < arg_count; i++) {
			LLVMTypeRef t = arg_types[i];
			LLVMTypeKind kind = LLVMGetTypeKind(t);
			if (kind == LLVMStructTypeKind) {
				i64 sz = lb_sizeof(t);
				if (sz == 0) {
					args[i] = lb_arg_type_ignore(t);
				} else {
					args[i] = lb_arg_type_indirect(t, lb_create_enum_attribute(c, "byval", true));
				}
			} else {
				args[i] = non_struct(c, t);
			}
		}
		return args;
	}

	lbArgType compute_return_type(LLVMContextRef c, LLVMTypeRef return_type, bool return_is_defined) {
		if (!return_is_defined) {
			return lb_arg_type_direct(LLVMVoidTypeInContext(c));
		} else if (lb_is_type_kind(return_type, LLVMStructTypeKind)) {
			i64 sz = lb_sizeof(return_type);
			switch (sz) {
			case 1: return lb_arg_type_direct(return_type, LLVMIntTypeInContext(c,  8), nullptr, nullptr);
			case 2: return lb_arg_type_direct(return_type, LLVMIntTypeInContext(c, 16), nullptr, nullptr);
			case 4: return lb_arg_type_direct(return_type, LLVMIntTypeInContext(c, 32), nullptr, nullptr);
			case 8: return lb_arg_type_direct(return_type, LLVMIntTypeInContext(c, 64), nullptr, nullptr);
			}
			return lb_arg_type_indirect(LLVMPointerType(return_type, 0), lb_create_enum_attribute(c, "sret", true));
		} else if (build_context.metrics.os == TargetOs_windows && lb_is_type_kind(return_type, LLVMIntegerTypeKind) && lb_sizeof(return_type) == 16) {
			return lb_arg_type_direct(return_type, LLVMIntTypeInContext(c, 128), nullptr, nullptr);
		}
		return non_struct(c, return_type);
	}
};




LB_ABI_INFO(lb_get_abi_info) {
	switch (calling_convention) {
	case ProcCC_None:
	case ProcCC_PureNone:
	case ProcCC_InlineAsm:
		{
			lbFunctionType *ft = gb_alloc_item(heap_allocator(), lbFunctionType);
			ft->ctx = c;
			ft->args = array_make<lbArgType>(heap_allocator(), arg_count);
			for (unsigned i = 0; i < arg_count; i++) {
				ft->args[i] = lb_arg_type_direct(arg_types[i]);
			}
			if (return_is_defined) {
				ft->ret = lb_arg_type_direct(return_type);
			} else {
				ft->ret = lb_arg_type_direct(LLVMVoidTypeInContext(c));
			}
			ft->calling_convention = calling_convention;
			return ft;
		}
	}

	if (build_context.metrics.arch == TargetArch_amd64) {
		if (build_context.metrics.os == TargetOs_windows) {
			return lbAbiAmd64Win64::abi_info(c, arg_types, arg_count, return_type, return_is_defined, calling_convention);
		} else {
			return lbAbiAmd64SysV::abi_info(c, arg_types, arg_count, return_type, return_is_defined, calling_convention);
		}
	} else if (build_context.metrics.arch == TargetArch_386) {
		return lbAbi386::abi_info(c, arg_types, arg_count, return_type, return_is_defined, calling_convention);
	} else if (build_context.metrics.arch == TargetArch_wasm32) {
		return lbAbi386::abi_info(c, arg_types, arg_count, return_type, return_is_defined, calling_convention);
	}
	GB_PANIC("Unsupported ABI");
	return {};
}