"""Embeds a file as a C byte array: embed.py <input> <symbol> <output.c>"""
import sys

data = open(sys.argv[1], 'rb').read()
name = sys.argv[2]
rows = ',\n'.join(','.join(str(b) for b in data[i:i + 32]) for i in range(0, len(data), 32))
open(sys.argv[3], 'w').write(f'const unsigned char {name}[]={{\n{rows}\n}};\nconst unsigned int {name}_size={len(data)};\n')
