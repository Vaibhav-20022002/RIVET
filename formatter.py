#!/usr/bin/env python3
import os
import sys
import subprocess

# Extensions to look for
EXTENSIONS = ('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp')

def get_files_to_format(src_dir):
    """Recursively collects all C/C++ source files, following symlinks."""
    matched_files = []
    # os.walk(..., followlinks=True) behaves like `find -L`
    for root, _, files in os.walk(src_dir, followlinks=True):
        for file in files:
            if file.lower().endswith(EXTENSIONS):
                matched_files.append(os.path.join(root, file))
    return matched_files

def transform_comments(file_path):
    """Applies the comment-style conversions line by line."""
    try:
        with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
            lines = f.readlines()
    except Exception as e:
        print(f"ERROR: Could not read {file_path}: {e}", file=sys.stderr)
        return False

    new_lines = []
    for line in lines:
        stripped = line.strip()
        
        # Step A: Globally swap ///< to //<
        if '///<' in line:
            line = line.replace('///<', '//<')
            stripped = line.strip()

        # Step B: Check if the line has actual code before a comment indicator
        # Find the first occurrence of '//'
        comment_idx = line.find('//')
        
        if comment_idx != -1:
            # Check if there is non-whitespace code before the '//'
            code_before = line[:comment_idx].strip()
            
            if code_before:
                # It's an inline comment on a line with code
                # Extract the comment portion
                comment_part = line[comment_idx:]
                
                # Normalise standard inline or triple slash to //<
                if comment_part.startswith('///') and not comment_part.startswith('///<'):
                    line = line[:comment_idx] + '//<' + comment_part[3:]
                elif comment_part.startswith('//') and not comment_part.startswith('//<'):
                    line = line[:comment_idx] + '//<' + comment_part[2:]
            else:
                # Step C: Pure comment line (only whitespace before '//')
                if stripped.startswith('//') and not stripped.startswith('///') and not stripped.startswith('//<'):
                    # Replace the first instance of // with ///
                    line = line.replace('//', '///', 1)

        new_lines.append(line)

    try:
        with open(file_path, 'w', encoding='utf-8') as f:
            f.writelines(new_lines)
        return True
    except Exception as e:
        print(f"ERROR: Could not write {file_path}: {e}", file=sys.stderr)
        return False

def main():
    # Target directory is the folder where this script is saved
    script_dir = os.path.dirname(os.path.abspath(__file__))
    
    # Verify clang-format exists in system path
    try:
        subprocess.run(['clang-format', '--version'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("ERROR: clang-format not found in PATH", file=sys.stderr)
        sys.exit(2)

    if not os.path.isdir(script_dir):
        print(f"ERROR: src directory not found: {script_dir}", file=sys.stderr)
        sys.exit(2)

    files = get_files_to_format(script_dir)
    total_files = len(files)
    
    print(f"Found {total_files} files to format.")
    if total_files == 0:
        sys.exit(0)

    formatted = 0
    failures = 0

    for f in files:
        if not os.access(f, os.R_OK):
            print(f"SKIP (unreadable): {f}", file=sys.stderr)
            failures += 1
            continue

        # Run clang-format command
        try:
            result = subprocess.run(
                ['clang-format', '--verbose', '-i', f],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )
            if result.returncode == 0:
                formatted += 1
            else:
                print(f"FAILED: clang-format on: {f}\n{result.stderr}", file=sys.stderr)
                failures += 1
                continue
        except Exception as e:
            print(f"FAILED: Execution error on {f}: {e}", file=sys.stderr)
            failures += 1
            continue

        # Run our custom comment parser
        if not transform_comments(f):
            failures += 1

    print(f"Result: formatted={formatted}, failed={failures}, total={total_files}")
    
    if failures > 0:
        sys.exit(1)
    sys.exit(0)

if __name__ == '__main__':
    main()

