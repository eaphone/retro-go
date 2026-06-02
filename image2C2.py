import os
import sys

if __name__ == "__main__" and len(sys.argv) > 1:
    if os.path.exists(sys.argv[1]):
        png_path=sys.argv[1]
    else:
        png_path=os.path.join(os.path.dirname(__file__), sys.argv[1])
    if not os.path.exists(png_path):
        print(f'File not found: {png_path}')
    else:
        with open(png_path, 'rb') as f:
            data = f.read()
        
        size = len(data)
        
        # Create C string literal with escaped bytes
        hex_str = ''
        for i, b in enumerate(data):
            hex_str += f'\\x{b:02X}'
        
        output = f'static const binfile_t {os.path.basename(sys.argv[1])} = {{"{os.path.basename(sys.argv[1])}", {size}, {hex_str}\n}};'
        
        print(output)