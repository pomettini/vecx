ENTRY(eventHandlerShim)
GROUP(libgcc.a libc.a libm.a)

SECTIONS
{
	.text :
	{
		*(.text)
		*(.text.*)

		KEEP(*(.init))
		KEEP(*(.fini))

		/* .ctors */
		*crtbegin.o(.ctors)
		*crtbegin?.o(.ctors)
		*(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
		*(SORT(.ctors.*))
		*(.ctors)

		/* .dtors */
 		*crtbegin.o(.dtors)
 		*crtbegin?.o(.dtors)
 		*(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
 		*(SORT(.dtors.*))
 		*(.dtors)

		*(.rodata*)

		/* runtime-relocatable hot code, bounded by __itcm_start/__itcm_end.
		 * kept INSIDE .text so its R_ARM_ABS32 relocations land in .rel.text
		 * and are applied by the loader (a separate .itcm output section is
		 * NOT relocated by the Playdate loader, leaving address literals bad).
		 */
		/* 32-byte aligned so the relocated copy can match the function's own
		 * -falign-functions=32 alignment (the decode is alignment-sensitive when
		 * executed from TCM; a mismatched offset costs ~12%). __itcm_start at 32
		 * makes the hot core's offset 0, and the runtime pool is 32-aligned too.
		 */
		. = ALIGN(16);
		__itcm_start = .;
		*(.itcm)
		*(.itcm.*)
		. = ALIGN(16);
		__itcm_end = .;

		KEEP(*(.eh_frame*))

	}

	.data :
	{
		__etext = .;

		__data_start__ = .;
		*(vtable)
		*(.data*)

		. = ALIGN(4);
		PROVIDE_HIDDEN (__preinit_array_start = .);
		KEEP(*(.preinit_array))
		PROVIDE_HIDDEN (__preinit_array_end = .);

		. = ALIGN(4);
		PROVIDE_HIDDEN (__init_array_start = .);
		KEEP(*(SORT(.init_array.*)))
		KEEP(*(.init_array))
		PROVIDE_HIDDEN (__init_array_end = .);

		. = ALIGN(4);
		PROVIDE_HIDDEN (__fini_array_start = .);
		KEEP(*(SORT(.fini_array.*)))
		KEEP(*(.fini_array))
		PROVIDE_HIDDEN (__fini_array_end = .);

		. = ALIGN(4);
		__data_end__ = .;

	}

	.bss :
	{
		. = ALIGN(4);
		__bss_start__ = .;
		*(.bss*)
		*(COMMON)
		*(COM)
		. = ALIGN(4);
		__bss_end__ = .;

	}

  /DISCARD/ :
  {
		*(.ARM.exidx)
  }

}
