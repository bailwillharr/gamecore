# vibecoded!

import os
import re
import sys

def extract_names(search_dir, output_file):
    pattern = re.compile(r'(?:gc::)?Name::createConstexpr\s*\(\s*"([^"]*)"')
    extensions = {'.cpp', '.h', '.hpp', '.c', '.cc', '.cxx'}
    unique_names = set()
    
    for root, _, files in os.walk(search_dir):
        for file in files:
            if os.path.splitext(file)[1] in extensions:
                file_path = os.path.join(root, file)
                try:
                    with open(file_path, 'r', encoding='utf-8') as f:
                        content = f.read()
                        unique_names.update(pattern.findall(content))
                except Exception as e:
                    print(f"Warning: Could not read {file_path}: {e}", file=sys.stderr)
    
    # Write directly to the output file, enforcing UTF-8 and standard newlines
    with open(output_file, 'w', encoding='utf-8', newline='\n') as out:
        for name in sorted(unique_names):
            out.write(f"{name}\n")

if __name__ == '__main__':
    search_directory = sys.argv[1] if len(sys.argv) > 1 else "."
    output_path = sys.argv[2] if len(sys.argv) > 2 else "name_strings.txt"
    extract_names(search_directory, output_path)