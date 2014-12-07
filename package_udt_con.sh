#!/bin/bash
tar cvf udt_con_x86.tar.bz2 --bzip2 -p --xform s:UDT_DLL/.bin/gmake/x86/release:: UDT_DLL/.bin/gmake/x86/release/UDT_cutter UDT_DLL/.bin/gmake/x86/release/UDT_splitter
tar cvf udt_con_x64.tar.bz2 --bzip2 -p --xform s:UDT_DLL/.bin/gmake/x64/release:: UDT_DLL/.bin/gmake/x64/release/UDT_cutter UDT_DLL/.bin/gmake/x64/release/UDT_splitter

