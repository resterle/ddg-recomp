# Counts magenta pixels (R, B > 150, G < 80) in each snapshot/*.bmp (or the files given).
# Used to measure the thin sky-line artifact; run from recomp/.
import struct, glob, sys
for f in sys.argv[1:] or sorted(glob.glob('snapshot/*.bmp')):
    d=open(f,'rb').read()
    off=struct.unpack_from('<I',d,10)[0]; w,hh=struct.unpack_from('<ii',d,18); bpp=struct.unpack_from('<H',d,28)[0]//8
    stride=(w*bpp+3)&~3; n=0
    for y in range(abs(hh)):
        r=d[off+y*stride:off+y*stride+w*bpp]
        for x in range(0,w*bpp,bpp):
            if r[x+2]>150 and r[x]>150 and r[x+1]<80: n+=1
    print(n, end=' ')
print()
