import struct
class Nif:
    def __init__(s,data):
        p=data.index(b'\n')+1;s.hdrline=data[:p]
        s.ver,s.endian,s.user,s.nblocks=struct.unpack_from('<IBII',data,p);p+=13
        s.bsver,=struct.unpack_from('<I',data,p);p+=4
        s.shorts=[]
        for _ in range(3):
            l=data[p];s.shorts.append(data[p+1:p+1+l]);p+=1+l
        nt,=struct.unpack_from('<H',data,p);p+=2
        s.types=[]
        for _ in range(nt):
            l,=struct.unpack_from('<I',data,p);s.types.append(data[p+4:p+4+l].decode());p+=4+l
        s.tidx=list(struct.unpack_from(f'<{s.nblocks}H',data,p));p+=2*s.nblocks
        sizes=list(struct.unpack_from(f'<{s.nblocks}I',data,p));p+=4*s.nblocks
        ns,s.maxlen=struct.unpack_from('<II',data,p);p+=8
        s.strings=[]
        for _ in range(ns):
            l,=struct.unpack_from('<I',data,p);s.strings.append(data[p+4:p+4+l].decode('latin1'));p+=4+l
        ng,=struct.unpack_from('<I',data,p);p+=4
        s.groups=list(struct.unpack_from(f'<{ng}I',data,p));p+=4*ng
        s.blocks=[]
        for sz in sizes:s.blocks.append(bytearray(data[p:p+sz]));p+=sz
        s.footer=data[p:]
    def type(s,i):return s.types[s.tidx[i]]
    def write(s):
        o=bytearray(s.hdrline)+struct.pack('<IBII',s.ver,s.endian,s.user,len(s.blocks))+struct.pack('<I',s.bsver)
        for sh in s.shorts:o+=bytes([len(sh)])+sh
        o+=struct.pack('<H',len(s.types))
        for t in s.types:o+=struct.pack('<I',len(t))+t.encode()
        o+=struct.pack(f'<{len(s.blocks)}H',*s.tidx)+struct.pack(f'<{len(s.blocks)}I',*[len(b) for b in s.blocks])
        o+=struct.pack('<II',len(s.strings),max([len(x) for x in s.strings]+[0]))
        for x in s.strings:o+=struct.pack('<I',len(x))+x.encode('latin1')
        o+=struct.pack('<I',len(s.groups))+struct.pack(f'<{len(s.groups)}I',*s.groups)
        for b in s.blocks:o+=b
        o+=s.footer;return bytes(o)
if __name__=='__main__':
    import sys
    n=Nif(open(sys.argv[1],'rb').read())
    print(n.hdrline,n.ver,n.user,n.bsver,n.shorts,n.strings)
    for i,b in enumerate(n.blocks):print(i,n.type(i),len(b))
    print('footer',n.footer.hex())
    assert n.write()==open(sys.argv[1],'rb').read();print('roundtrip ok')
