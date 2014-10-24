/* -----------------------------------------------------------------------
   ffi.c - Copyright (c) 2011, 2013 Anthony Green
           Copyright (c) 1996, 2003-2004, 2007-2008 Red Hat, Inc.

   SPARC Foreign Function Interface

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   ``Software''), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be included
   in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED ``AS IS'', WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
   HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
   DEALINGS IN THE SOFTWARE.
   ----------------------------------------------------------------------- */

#include <ffi.h>
#include <ffi_common.h>
#include <stdlib.h>
#include "internal.h"

/* Force FFI_TYPE_LONGDOUBLE to be different than FFI_TYPE_DOUBLE;
   all further uses in this file will refer to the 128-bit type.  */
#if FFI_TYPE_LONGDOUBLE != FFI_TYPE_DOUBLE
# if FFI_TYPE_LONGDOUBLE != 4
#  error FFI_TYPE_LONGDOUBLE out of date
# endif
#else
# undef FFI_TYPE_LONGDOUBLE
# define FFI_TYPE_LONGDOUBLE 4
#endif

#ifdef SPARC64
/* Perform machine dependent cif processing */

int FFI_HIDDEN
ffi_v9_layout_struct (ffi_type *arg, int off, void *d, void *si, void *sf)
{
  ffi_type **elts, *t;

  for (elts = arg->elements; (t = *elts) != NULL; elts++)
    {
      size_t z = t->size;
      void *src = si;

      off = ALIGN(off, t->alignment);
      switch (t->type)
	{
	case FFI_TYPE_STRUCT:
	  off = ffi_v9_layout_struct(t, off, d, si, sf);
	  off = ALIGN(off, FFI_SIZEOF_ARG);
	  continue;
	case FFI_TYPE_FLOAT:
	case FFI_TYPE_DOUBLE:
	case FFI_TYPE_LONGDOUBLE:
	  /* Note that closures start with the argument offset,
	     so that we know when to stop looking at fp regs.  */
	  if (off < 128)
	    src = sf;
	  break;
	}
      memcpy(d + off, src + off, z);
      off += z;
    }

  return off;
}

ffi_status FFI_HIDDEN
ffi_prep_cif_machdep(ffi_cif *cif)
{
  ffi_type *rtype = cif->rtype;
  int rtt = rtype->type;
  size_t bytes = 0;
  int i, n, flags;

  /* Set the return type flag */
  switch (rtt)
    {
    case FFI_TYPE_VOID:
      flags = SPARC_RET_VOID;
      break;
    case FFI_TYPE_FLOAT:
      flags = SPARC_RET_FLOAT;
      break;
    case FFI_TYPE_DOUBLE:
      flags = SPARC_RET_DOUBLE;
      break;
    case FFI_TYPE_LONGDOUBLE:
      flags = SPARC_RET_LDOUBLE;
      break;

    case FFI_TYPE_STRUCT:
      if (rtype->size > 32)
	{
	  flags = SPARC_RET_VOID | SPARC_FLAG_RET_IN_MEM;
	  bytes = 8;
	}
      else
	flags = SPARC_RET_STRUCT;
      break;

    case FFI_TYPE_SINT8:
      flags = SPARC_RET_SINT8;
      break;
    case FFI_TYPE_UINT8:
      flags = SPARC_RET_UINT8;
      break;
    case FFI_TYPE_SINT16:
      flags = SPARC_RET_SINT16;
      break;
    case FFI_TYPE_UINT16:
      flags = SPARC_RET_UINT16;
      break;
    case FFI_TYPE_INT:
    case FFI_TYPE_SINT32:
      flags = SPARC_RET_SINT32;
      break;
    case FFI_TYPE_UINT32:
      flags = SPARC_RET_UINT32;
      break;
    case FFI_TYPE_SINT64:
    case FFI_TYPE_UINT64:
    case FFI_TYPE_POINTER:
      flags = SPARC_RET_INT64;
      break;

    default:
      abort();
    }

  bytes = 0;
  for (i = 0, n = cif->nargs; i < n; ++i)
    {
      ffi_type *ty = cif->arg_types[i];
      size_t z = ty->size;
      size_t a = ty->alignment;

      switch (ty->type)
	{
	case FFI_TYPE_STRUCT:
	  /* Large structs passed by reference.  */
	  if (z > 16)
	    {
	      a = z = 8;
	      break;
	    }
	  /* ??? FALLTHRU -- check for fp members in the struct.  */
	case FFI_TYPE_FLOAT:
	case FFI_TYPE_DOUBLE:
	case FFI_TYPE_LONGDOUBLE:
	  flags |= SPARC_FLAG_FP_ARGS;
	  break;
	}
      bytes = ALIGN(bytes, a);
      bytes += ALIGN(z, 8);
    }

  /* Sparc call frames require that space is allocated for 6 args,
     even if they aren't used. Make that space if necessary. */
  if (bytes < 6 * 8)
    bytes = 6 * 8;

  /* The stack must be 2 word aligned, so round bytes up appropriately. */
  bytes = ALIGN(bytes, 16);

  /* Include the call frame to prep_args.  */
  bytes += 8*16 + 8*8;

  cif->bytes = bytes;
  cif->flags = flags;
  return FFI_OK;
}

extern void ffi_call_v9(ffi_cif *cif, void (*fn)(void), void *rvalue,
			void **avalue, size_t bytes) FFI_HIDDEN;

/* ffi_prep_args is called by the assembly routine once stack space
   has been allocated for the function's arguments */

int FFI_HIDDEN
ffi_prep_args_v9(ffi_cif *cif, unsigned long *argp, void *rvalue, void **avalue)
{
  ffi_type **p_arg;
  int flags = cif->flags;
  int i, nargs;

  if (rvalue == NULL)
    {
      if (flags & SPARC_FLAG_RET_IN_MEM)
	{
	  /* Since we pass the pointer to the callee, we need a value.
	     We allowed for this space in ffi_call, before ffi_call_v8
	     alloca'd the space.  */
	  rvalue = (char *)argp + cif->bytes;
	}
      else
	{
	  /* Otherwise, we can ignore the return value.  */
	  flags = SPARC_RET_VOID;
	}
    }

#ifdef USING_PURIFY
  /* Purify will probably complain in our assembly routine,
     unless we zero out this memory. */
  memset(argp, 0, 6*8);
#endif

  if (flags & SPARC_FLAG_RET_IN_MEM)
    *argp++ = (unsigned long)rvalue;

  p_arg = cif->arg_types;
  for (i = 0, nargs = cif->nargs; i < nargs; i++)
    {
      ffi_type *ty = p_arg[i];
      void *a = avalue[i];
      size_t z;

      switch (ty->type)
	{
	case FFI_TYPE_SINT8:
	  *argp++ = *(SINT8 *)a;
	  break;
	case FFI_TYPE_UINT8:
	  *argp++ = *(UINT8 *)a;
	  break;
	case FFI_TYPE_SINT16:
	  *argp++ = *(SINT16 *)a;
	  break;
	case FFI_TYPE_UINT16:
	  *argp++ = *(UINT16 *)a;
	  break;
	case FFI_TYPE_INT:
	case FFI_TYPE_SINT32:
	  *argp++ = *(SINT32 *)a;
	  break;
	case FFI_TYPE_UINT32:
	case FFI_TYPE_FLOAT:
	  *argp++ = *(UINT32 *)a;
	  break;
	case FFI_TYPE_SINT64:
	case FFI_TYPE_UINT64:
	case FFI_TYPE_POINTER:
	case FFI_TYPE_DOUBLE:
	  *argp++ = *(UINT64 *)a;
	  break;

	case FFI_TYPE_LONGDOUBLE:
	case FFI_TYPE_STRUCT:
	  z = ty->size;
	  if (z > 16)
	    {
	      /* For structures larger than 16 bytes we pass reference.  */
	      *argp++ = (unsigned long)a;
	      break;
	    }
	  if (((unsigned long)argp & 15) && ty->alignment > 8)
	    argp++;
	  memcpy(argp, a, z);
	  argp += ALIGN(z, 8) / 8;
	  break;

	default:
	  abort();
	}
    }

  return flags;
}

void
ffi_call(ffi_cif *cif, void (*fn)(void), void *rvalue, void **avalue)
{
  size_t bytes = cif->bytes;

  FFI_ASSERT (cif->abi == FFI_V9);

  if (rvalue == NULL && (cif->flags & SPARC_FLAG_RET_IN_MEM))
    bytes += ALIGN (cif->rtype->size, 16);

  ffi_call_v9(cif, fn, rvalue, avalue, -bytes);
}

#ifdef __GNUC__
static inline void
ffi_flush_icache (void *p)
{
  asm volatile ("flush	%0; flush %0+8" : : "r" (p) : "memory");
}
#else
extern void ffi_flush_icache (void *) FFI_HIDDEN;
#endif

extern void ffi_closure_v9(void) FFI_HIDDEN;

ffi_status
ffi_prep_closure_loc (ffi_closure* closure,
		      ffi_cif* cif,
		      void (*fun)(ffi_cif*, void*, void**, void*),
		      void *user_data,
		      void *codeloc)
{
  unsigned int *tramp = (unsigned int *) &closure->tramp[0];
  unsigned long fn;

  if (cif->abi != FFI_V9)
    return FFI_BAD_ABI;

  /* Trampoline address is equal to the closure address.  We take advantage
     of that to reduce the trampoline size by 8 bytes. */
  fn = (unsigned long) ffi_closure_v9;
  tramp[0] = 0x83414000;	/* rd	%pc, %g1	*/
  tramp[1] = 0xca586010;	/* ldx	[%g1+16], %g5	*/
  tramp[2] = 0x81c14000;	/* jmp	%g5		*/
  tramp[3] = 0x01000000;	/* nop			*/
  *((unsigned long *) &tramp[4]) = fn;

  closure->cif = cif;
  closure->fun = fun;
  closure->user_data = user_data;

  ffi_flush_icache (closure);

  return FFI_OK;
}

int FFI_HIDDEN
ffi_closure_sparc_inner_v9(ffi_closure *closure, void *rvalue,
			   unsigned long *gpr, unsigned long *fpr)
{
  ffi_cif *cif;
  ffi_type **arg_types;
  void **avalue;
  int i, argn, nargs, flags;

  cif = closure->cif;
  arg_types = cif->arg_types;
  nargs = cif->nargs;
  flags = cif->flags;

  avalue = alloca(nargs * sizeof(void *));

  /* Copy the caller's structure return address so that the closure
     returns the data directly to the caller.  */
  if (flags & SPARC_FLAG_RET_IN_MEM)
    {
      rvalue = (void *) gpr[0];
      /* Skip the structure return address.  */
      argn = 1;
    }
  else
    argn = 0;

  /* Grab the addresses of the arguments from the stack frame.  */
  for (i = 0; i < nargs; i++)
    {
      ffi_type *ty = arg_types[i];
      void *a = &gpr[argn++];
      size_t z;

      switch (ty->type)
	{
	case FFI_TYPE_STRUCT:
	  z = ty->size;
	  if (z > 16)
	    a = *(void **)a;
	  else
	    {
	      if (--argn < 16)
	        ffi_v9_layout_struct(arg_types[i], 8*argn, gpr, gpr, fpr);
	      argn += ALIGN (z, 8) / 8;
	    }
	  break;

	case FFI_TYPE_LONGDOUBLE:
	  if (--argn & 1)
	    argn++;
	  a = (argn < 16 ? fpr : gpr) + argn;
	  argn += 2;
	  break;
	case FFI_TYPE_DOUBLE:
	  if (argn <= 16)
	    a = fpr + argn - 1;
	  break;
	case FFI_TYPE_FLOAT:
	  if (argn <= 16)
	    a = fpr + argn - 1;
	  a += 4;
	  break;

	case FFI_TYPE_UINT64:
	case FFI_TYPE_SINT64:
	case FFI_TYPE_POINTER:
	  break;
	case FFI_TYPE_INT:
	case FFI_TYPE_UINT32:
	case FFI_TYPE_SINT32:
	  a += 4;
	  break;
        case FFI_TYPE_UINT16:
        case FFI_TYPE_SINT16:
	  a += 6;
	  break;
        case FFI_TYPE_UINT8:
        case FFI_TYPE_SINT8:
	  a += 7;
	  break;

	default:
	  abort();
	}
      avalue[i] = a;
    }

  /* Invoke the closure.  */
  (closure->fun) (cif, rvalue, avalue, closure->user_data);

  /* Tell ffi_closure_sparc how to perform return type promotions.  */
  return flags;
}
#endif /* SPARC64 */
