#!/bin/sh

rename_files_in_folder() {
	ls | cat -n | while read n f; do n="$( printf '%03d' "$n" )"; mv "$f" "$n.mp3"; done
}

for d in ./*/ ; do
	pushd $d
	rename_files_in_folder
	popd
done
