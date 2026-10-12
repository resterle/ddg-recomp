# usage: python3 montage.py out.png in1.png in2.png ... (side by side, via sips->bmp)
import subprocess, struct, zlib, sys
imgs=[]
for f in sys.argv[2:]:
    subprocess.run(['sips','-s','format','bmp',f,'--out',f+'.bmp'],capture_output=True)
    d=open(f+'.bmp','rb').read()
    off=struct.unpack_from('<I',d,10)[0]; w,h=struct.unpack_from('<ii',d,18); bpp=struct.unpack_from('<H',d,28)[0]//8
    stride=(w*bpp+3)&~3; rows=[]
    for y in range(abs(h)):
        sy=abs(h)-1-y if h>0 else y
        r=d[off+sy*stride:off+sy*stride+w*bpp]
        rows.append(bytes(b for x in range(w) for b in (r[x*bpp+2],r[x*bpp+1],r[x*bpp])))
    imgs.append((w,rows))
H=max(len(r) for _,r in imgs); W=sum(w+4 for w,_ in imgs); raw=b''
for y in range(H):
    line=b''
    for w,rows in imgs: line+=(rows[y] if y<len(rows) else b'\0'*3*w)+b'\xff'*12
    raw+=b'\0'+line
def ch(t,c): return struct.pack('>I',len(c))+t+c+struct.pack('>I',zlib.crc32(t+c)&0xffffffff)
open(sys.argv[1],'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',W,H,8,2,0,0,0))+ch(b'IDAT',zlib.compress(raw))+ch(b'IEND',b''))
