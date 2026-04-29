#!/usr/bin/env python3
"""simple brainfuck interpreter in python




THIS FILE IS THE PART OF MOFL PROJECT (see github.com/bogdan0759/mofl)


"""

import sys
import argparse
from typing import List, Tuple, Union


class PyBF:
    

    
    OP_PTR_CHANGE = 0  # move pointer 
    OP_VAL_CHANGE = 1  # change current cell 
    OP_OUTPUT     = 2  # print char
    OP_INPUT      = 3  # read char
    OP_JUMP_IF_0  = 4  # jump forward if 0 
    OP_JUMP_IF_NZ = 5  # jump backward if not 0 
    OP_CLEAR      = 6  # clear cell to 0
    OP_ADD_MOVE   = 7  # move/add value to another cell 

    def __init__(self, code: str, memory_size: int = 30000):
        self.raw_code = "".join(c for c in code if c in "><+-.,[]")
        self.memory = [0] * memory_size
        self.ptr = 0
        self.pc = 0
        self.ops = self._compile()

    def _compile(self) -> List[Union[Tuple[int, int], Tuple[int, Tuple[int, int]]]]:
        ops = []
        i = 0
        n = len(self.raw_code)

        while i < n:
            char = self.raw_code[i]

            if char in "+-":
                count = 0
                while i < n and self.raw_code[i] in "+-":
                    count += 1 if self.raw_code[i] == "+" else -1
                    i += 1
                if count % 256 != 0:
                    ops.append((self.OP_VAL_CHANGE, count))
                continue
            
            elif char in "><":
                count = 0
                while i < n and self.raw_code[i] in "><":
                    count += 1 if self.raw_code[i] == ">" else -1
                    i += 1
                if count != 0:
                    ops.append((self.OP_PTR_CHANGE, count))
                continue
            
            elif char == ".":
                ops.append((self.OP_OUTPUT, 0))
            
            elif char == ",":
                ops.append((self.OP_INPUT, 0))
            
            elif char == "[":
                if i + 2 < n and self.raw_code[i+1] in "+-" and self.raw_code[i+2] == "]":
                    ops.append((self.OP_CLEAR, 0))
                    i += 3
                    continue
                
                
                ops.append((self.OP_JUMP_IF_0, 0)) 
            
            elif char == "]":
                ops.append((self.OP_JUMP_IF_NZ, 0)) 
            
            i += 1

 
        stack = []
        for idx, (op_type, _) in enumerate(ops):
            if op_type == self.OP_JUMP_IF_0:
                stack.append(idx)
            elif op_type == self.OP_JUMP_IF_NZ:
                if not stack:
                    raise SyntaxError("mismatched ] (unopen loop)")
                start_idx = stack.pop()
                ops[start_idx] = (self.OP_JUMP_IF_0, idx)
                ops[idx] = (self.OP_JUMP_IF_NZ, start_idx)
        
        if stack:
            raise SyntaxError("mismatched [ (unclosed loop)")

        return ops

    def run(self):
        ops_len = len(self.ops)
        mem_len = len(self.memory)

        while self.pc < ops_len:
            op, val = self.ops[self.pc]

            if op == self.OP_VAL_CHANGE:
                self.memory[self.ptr] = (self.memory[self.ptr] + val) % 256
            
            elif op == self.OP_PTR_CHANGE:
                self.ptr = (self.ptr + val) % mem_len
            
            elif op == self.OP_OUTPUT:
                sys.stdout.write(chr(self.memory[self.ptr]))
                sys.stdout.flush()
            
            elif op == self.OP_INPUT:
                char = sys.stdin.read(1)
                self.memory[self.ptr] = ord(char) if char else 0
            
            elif op == self.OP_JUMP_IF_0:
                if self.memory[self.ptr] == 0:
                    self.pc = val
            
            elif op == self.OP_JUMP_IF_NZ:
                if self.memory[self.ptr] != 0:
                    self.pc = val
            
            elif op == self.OP_CLEAR:
                self.memory[self.ptr] = 0
            
            self.pc += 1


def main():
    parser = argparse.ArgumentParser(
        description="PyBF",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("source", help=" path .bf file or code")
    parser.add_argument("-m", "--memory", type=int, default=30000, help="memory size")
    parser.add_argument("-e", "--evaluate", action="store_true", help="runs raw code")
    
    args = parser.parse_args()

    content = ""
    if args.evaluate:
        content = args.source
    elif args.source.endswith(".bf"):
        try:
            with open(args.source, "r") as f:
                content = f.read()
        except FileNotFoundError:
            print(f"\033[91merror file {args.source} doesnt exist\033[0m")
            sys.exit(1)
    else:
        try:
            with open(args.source, "r") as f:
                content = f.read()
        except (FileNotFoundError, OSError):
            content = args.source

    if not content:
        print("\033[93mUse -h for help\033[0m")
        return

    interpreter = PyBF(content, memory_size=args.memory)
    try:
        interpreter.run()
    except KeyboardInterrupt:
        print("\n\033[93m[interrupt]\033[0m")
    except Exception as e:
        print(f"\n\033[91m[runtime (python) error]: {e}\033[0m")
        sys.exit(1)


if __name__ == "__main__":
    main()