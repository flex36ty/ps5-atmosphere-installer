import pathlib, struct, hashlib, base64, sys
b=pathlib.Path(sys.argv[1]).read_bytes()
u=lambda f,o:struct.unpack_from('<'+f,b,o)[0]
ph=[u('Q',32)+i*u('H',54) for i in range(u('H',56))]
def off(a):
    return next(u('Q',p+8)+a-u('Q',p+16) for p in ph if u('I',p)==1 and u('Q',p+16)<=a<u('Q',p+16)+u('Q',p+32))
p=next(p for p in ph if u('I',p)==2)
pairs=[(u('Q',o),u('Q',o+8)) for o in range(u('Q',p+8),u('Q',p+8)+u('Q',p+32),16)]
t=dict(pairs); st=off(t[5]); sy=off(t[6]); h=off(t[4]); nb=u('I',h); nc=u('I',h+4)
def string(n):return b[st+n:b.index(0,st+n)].decode()
mods={v>>48:string(v&0xffffffff) for k,v in pairs if k in (0x61000043,0x61000045)}
libs={v>>48:string(v&0xffffffff) for k,v in pairs if k in (0x61000047,0x61000049)}
alphabet='ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-'
def decode(s):
    n=0
    for c in s:n=n*64+alphabet.index(c)
    return n
def elfhash(s):
    n=0
    for c in s.encode():
        n=(n<<4)+c; g=n&0xf0000000
        if g:n^=g>>24
        n&=~g
    return n
names=['atmosphere_backend_run','atmosphere_request_stop','atmosphere_backend_stage','atmosphere_native_request','atmosphere_native_free']
addresses={}
for name in names:
    nid=base64.b64encode(hashlib.sha1(name.encode()+bytes.fromhex('518d64a635ded8c1e6b039b1c3e55230')).digest()[:8][::-1]).decode()[:11].replace('/','-')
    matches=[]
    for i in range(1,nc):
        s=string(u('I',sy+i*24)); parts=s.split('#')
        if parts[0]!=nid or not u('H',sy+i*24+6):continue
        key=parts[0]+'#'+libs[decode(parts[1])]+'#'+mods[decode(parts[2])]
        pos=u('I',h+8+4*(elfhash(key)%nb));seen=set()
        while pos and pos not in seen:
            if pos==i:matches.append(i);break
            seen.add(pos);pos=u('I',h+8+nb*4+pos*4)
        print(name,s,key,'address',hex(u('Q',sy+i*24+8)),'reachable',bool(matches))
        addresses[name]=u('Q',sy+i*24+8)
    assert len(matches)==1,(name,matches)
print('All five named exports are present and reachable through the symbol hash table.')
if len(sys.argv)>2:
    obj=pathlib.Path(sys.argv[2]).read_bytes()
    r=lambda f,o:struct.unpack_from('<'+f,obj,o)[0]
    sections=[r('Q',40)+i*r('H',58) for i in range(r('H',60))]
    symbols={}
    for section in sections:
        if r('I',section+4)!=2:continue
        strings=r('Q',sections[r('I',section+40)]+24)
        for p in range(r('Q',section+24),r('Q',section+24)+r('Q',section+32),24):
            start=strings+r('I',p)
            name=obj[start:obj.index(0,start)].decode()
            if r('H',p+6):symbols[name]=(r('H',p+6),r('Q',p+8))
    start_section,start_offset=symbols['atmosphere_module_start']
    code_offset=r('Q',sections[start_section]+24)+start_offset
    # The validation prologue has no relocations; its first 16 bytes must match
    # the input startup function rather than the old synthetic return-zero stub.
    assert b[off(t[12]):off(t[12])+16]==obj[code_offset:code_offset+16]
    assert b[off(t[12]):off(t[12])+3]!=bytes.fromhex('31c0c3')
    print('DT_INIT matches atmosphere_module_start validation prologue:',hex(t[12]))
