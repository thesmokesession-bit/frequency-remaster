/* The title's layout: PS5_Vulkan's (tooling/psbc/ps5-pie-unwind.ld over tooling/native/ps5-pie.ld),
 * with the names host/core/main.cpp and src/os/mem.cpp read the static data's bounds by, as the
 * Linux start-up files give them: the writable segment starts with .dynamic and ends with .bss. */
INCLUDE ps5-pie-unwind.ld
PROVIDE(__data_start = ADDR(.dynamic));
PROVIDE(_end = ADDR(.bss) + SIZEOF(.bss));
