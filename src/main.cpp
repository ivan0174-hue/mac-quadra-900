/*
 *  main.cpp - Circle entry point for QuadraPi
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "kernel.h"
#include <circle/startup.h>

int main(void)
{
	// The kernel object must never be destroyed (devices keep pointers to it)
	static CKernel Kernel;
	if (!Kernel.Initialize()) {
		halt();
		return EXIT_HALT;
	}

	Kernel.Run();	// does not return

	halt();
	return EXIT_HALT;
}
