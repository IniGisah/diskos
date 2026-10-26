/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The finished stock-library helper ELF travels inside mq_ui's signed bytes.
 * AUDIO_STOCK_IMAGE_RAW names the file tools/embed_audiodec.py generated from
 * the helper: a 48-byte descriptor (magic, length, zero, SHA-256) followed by
 * the exact helper bytes. The helper is data only: audio_stock_process.h copies
 * it to a private file, verifies the copy and executes that copy. */
#ifndef AUDIO_STOCK_IMAGE_RAW
#error "AUDIO_STOCK_IMAGE_RAW must name the generated helper image"
#endif
	.section .diskos_audiodec,"a",%progbits
	.globl diskos_audiodec_image
	.globl diskos_audiodec_image_end
	.type diskos_audiodec_image,%object
diskos_audiodec_image:
	.incbin AUDIO_STOCK_IMAGE_RAW, 48
diskos_audiodec_image_end:
	.size diskos_audiodec_image, diskos_audiodec_image_end - diskos_audiodec_image

	.section .rodata.diskos_audiodec_desc,"a",%progbits
	.balign 4
	.globl diskos_audiodec_desc
	.type diskos_audiodec_desc,%object
diskos_audiodec_desc:
	.incbin AUDIO_STOCK_IMAGE_RAW, 0, 48
	/* The number of bytes actually included; must equal the generated length. */
	.4byte diskos_audiodec_image_end - diskos_audiodec_image
	.size diskos_audiodec_desc, 52

	.section .note.GNU-stack,"",%progbits
