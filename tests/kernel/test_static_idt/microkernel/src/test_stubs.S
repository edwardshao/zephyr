/* test_stubs.S - Exception and interrupt stubs */

/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
DESCRIPTION
This module implements assembler exception and interrupt stubs for regression
testing.
 */

#define _ASMLANGUAGE

#ifdef CONFIG_ISA_IA32

/* IA-32 specific */

#include <arch/cpu.h>
#include <nano_private.h>
#include <arch/x86/asm.h>
#include <asm_inline.h>

	/* exports (internal APIs) */

	GTEXT(_ExcEnt)
	GTEXT(_ExcExit)
	GTEXT(_IntEnt)
	GTEXT(_IntExit)

/* Static interrupt handler stubs */

	GTEXT(nanoIntStub)
SECTION_FUNC(TEXT, nanoIntStub)
        call    _IntEnt
#ifdef CONFIG_X86_IAMCU
	movl	$0, %eax
#else
        pushl   $0
#endif
        call    isr_handler
#ifndef CONFIG_X86_IAMCU
        addl    $4, %esp
#endif
        jmp     _IntExit
#else

#error Arch not supported

#endif /* CONFIG_ISA_IA32 */
