/* atmosphere.elf carries the runtime it saves on the console (build/atmosphere_runtime.elf, the same
 * server and icon setup without this copy). Payload managers then start the saved runtime. */
extern const unsigned char atmosphere_runtime_image[], atmosphere_runtime_image_end[];
__asm__(".section .rodata\n"
        ".global atmosphere_runtime_image\n"
        ".global atmosphere_runtime_image_end\n"
        ".balign 16\n"
        "atmosphere_runtime_image:\n"
        ".incbin \"build/atmosphere_runtime.elf\"\n"
        "atmosphere_runtime_image_end:\n"
        ".previous\n");
