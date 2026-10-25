"""SHA-pinned original HD option stores, audio channel callbacks and config writer."""
import argparse
import json
import subprocess
from unicorn.x86_const import UC_X86_REG_EAX,UC_X86_REG_ECX,UC_X86_REG_ESP,UC_X86_REG_EIP
from test_game_text import Editor
from build_android import ROOT,CLANG


def proof():
    e=Editor('stronghold');u=e.u;sound=0xBC8AE0;events=[]
    def ret(value=0,pop=0):
        sp=u.reg_read(UC_X86_REG_ESP);u.reg_write(UC_X86_REG_EAX,value)
        u.reg_write(UC_X86_REG_EIP,e.r(sp));u.reg_write(UC_X86_REG_ESP,sp+4+pop)
    # The real original close shifts the history and requests the previous
    # original dialog; only the UI renderer/state-dispatch leaf is intercepted.
    def close(pc):
        if pc==0x48E2F0:
            sp=u.reg_read(UC_X86_REG_ESP);events.append(('dialog',e.r(sp+4),e.r(sp+8)));ret(pop=8)
    # The original slider47B7B0 reports10..90 (both SP and MP). Keyboard
    # decrement49364F independently subtracts5 and clamps at10.
    low,high,current=0x10011000,0x10011004,0x10011008
    slider_limits=[]
    for multiplayer in(0,1):
        e.w(0xF671D0,multiplayer);e.w(0xBC72F0,40);e.w(0x1005304,45)
        e.run(0x47B7B0,(0,1,low,high,current))
        assert e.r(low)==10 and e.r(high)==90
        slider_limits.append(dict(multiplayer=multiplayer,minimum=e.r(low),maximum=e.r(high)))
    e.w(0xF671D0,0);e.w(current,10);e.run(0x47B7B0,(0,3,low,high,current))
    assert e.r(0xBC72F0)==10
    keyboard=[]
    for before,after in((10,10),(11,10),(15,10),(20,15),(90,85)):
        e.w(0x1503DE4,before)
        def decrement(pc):
            if pc in(0x493602,0x49314F):u.emu_stop()
        e.run(0x49364F,hook=decrement);assert e.r(0x1503DE4)==after
        keyboard.append(dict(before=before,after=after))
    e.w(0xBC7314,13);e.w(0xBC7318,11);e.w(0xBC72F8,0)
    e.run(0x47D290,(18,),hook=close)
    assert e.r(0x1503DE4)==10 and e.r(0x1503DE8)==0 and e.r(0xBC7314)==11
    assert events==[('dialog',11,1)]
    # Graphics apply proves scroll and cursor addresses separately from the
    # neighbouring zoom setting, executing the real cursor-sprite refresh.
    e.w(0xB18324,1);e.w(0xBC72E8,1);e.w(0x16E93E4,0);e.w(0xBC72EC,0)
    e.w(0xBC730C,1);e.w(0xABC54C,2);e.w(0xBC72F4,2)
    e.w(0xABC368+134*4,123);e.w(0xBC7314,6);e.w(0xBC7318,11)
    e.run(0x47D369,hook=close)
    assert e.r(0xBCAFA8)==2 and e.r(0xABC54C)==1 and e.r(0xABC368+124*4)==123
    assert e.r(0x16E93E4)==0
    # Execute original slider47BF60 -> volume4680F0 and stub only Miles API
    # calls. This proves the music/effects/speech channel fanout and values.
    api={}
    for dll in e.pe.DIRECTORY_ENTRY_IMPORT:
        for item in dll.imports:
            if item.name and (b'AIL_set_sample_volume' in item.name or b'AIL_set_stream_volume' in item.name or b'AIL_end_sample' in item.name):
                target=e.end+16+len(api)*16;api[target]=item.name.decode();e.w(item.address,target)
    assert len(api)==3,api
    def miles(pc):
        if pc in api:
            name=api[pc];sp=u.reg_read(UC_X86_REG_ESP)
            events.append((name,e.r(sp+4),e.r(sp+8) if 'volume' in name else None))
            ret(pop=8 if 'volume' in name else 4)
    e.w(sound+8,1)
    for type in range(5):e.w(sound+4*(type+8),1);e.w(sound+4*(type+23),100);e.w(sound+4*(type+3),1000+type)
    e.w(sound+92*4,1000)
    for i in range(31):e.w(sound+(100+i)*4,2000+i);e.w(sound+(2266+i)*4,100)
    args1,args2,value=0x10010000,0x10010004,0x10010008;channels=[]
    for category,amount,count in((0,50,1),(1,60,33),(2,70,2)):
        e.w(value,amount);events.clear();e.run(0x47BF60,(category,3,args1,args2,value),hook=miles)
        assert len(events)==count and all(event[2]==amount for event in events)
        channels.append(dict(category=category,amount=amount,original_miles_calls=count))
    assert [e.r(sound+i) for i in(0x70,0x74,0x78,0x7C,0x80)]==[50,60,60,70,70]
    assert e.r(sound+2264*4)==60
    # Mute/resume preserve original channel-stop and cached music logic.
    stopped=[];e.w(sound,1);e.w(sound+2263*4,2)
    def mute(pc):
        if pc==0x467C30:
            sp=u.reg_read(UC_X86_REG_ESP);stopped.append(e.r(sp+4));ret(pop=4)
        else:miles(pc)
    events.clear();e.run(0x4681C0,this=sound,hook=mute)
    assert e.r(sound)==0 and stopped==list(range(5)) and len(events)==31
    resumed=[]
    def resume(pc):
        if pc==0x469940:
            sp=u.reg_read(UC_X86_REG_ESP);resumed.append((u.reg_read(UC_X86_REG_ECX),e.r(sp+4),e.r(sp+8)));ret(pop=8)
    e.run(0x469A50,this=sound,hook=resume)
    assert e.r(sound)==1 and resumed==[(sound,sound+0x84,e.r(sound+0x34))]
    # Run the complete original config writer. Filesystem/CRT leaves are
    # captured, so no file is created. Original field pointers and bytes run.
    written=[]
    def save(pc):
        sp=u.reg_read(UC_X86_REG_ESP)
        if pc==0x465C50:
            obj=e.r(sp+4);u.mem_write(obj,bytes(28));e.h(obj+4,ord('X'));e.w(obj+24,7);ret(obj,pop=8)
        elif pc==0x53A0EB:ret(77)
        elif pc==0x537600:
            pointer,size,count,handle=(e.r(sp+4+i*4) for i in range(4));assert handle==77
            written.append((pointer,size,count,bytes(u.mem_read(pointer,size*count)).hex()));ret(count)
        elif pc in(0x536917,0x5363BA):ret()
    e.run(0x47F2C0,hook=save)
    fields=[0x1503DE4,0x1503DE8,0xBCAFA8,0xABC54C,sound,sound+0x70,sound+0x74,sound+0x7C]
    for address in fields:
        assert any(pointer==address and size==4 and count==1 and value==bytes(u.mem_read(address,4)).hex()
                   for pointer,size,count,value in written),(hex(address),written)
    assert u.reg_read(UC_X86_REG_ESP)==e.stack+4
    return dict(sha256=e.sha,scope='Real original speed/help stores, scroll/cursor apply+sprite refresh, history close, audio slider fanout/mute/resume, and complete config writer; only external Miles/CRT/filesystem/UI-dispatch leaves intercepted.',
                speed_slider_limits=slider_limits,keyboard_decrement=keyboard,
                addresses=[hex(a) for a in fields],audio_channels=channels,mute_streams=stopped,mute_samples=31,
                config_original_writes=len(written),config_cdecl_stack='PASS',no_genie=True)


def main():
    p=argparse.ArgumentParser();p.add_argument('--serial');args=p.parse_args();result=proof()
    if args.serial:
        binary=ROOT/'build/original-options-selfcheck'
        subprocess.run([str(CLANG),'--target=aarch64-linux-android30','-O2','-Iandroid/runtime','-Iandroid/host',
            'tests/original_options_selfcheck.c','-Wl,-z,max-page-size=16384','-o',str(binary)],cwd=ROOT,check=True)
        target='/data/local/tmp/original-options-selfcheck';adb=['adb','-s',args.serial]
        subprocess.run([*adb,'push',str(binary),target],capture_output=True,check=True)
        subprocess.run([*adb,'shell','chmod','755',target],capture_output=True,check=True)
        run=subprocess.run([*adb,'shell',target],capture_output=True,text=True,check=True)
        assert 'PASS' in run.stdout;result['native']=run.stdout.strip()
        subprocess.run([*adb,'shell','rm',target],capture_output=True,check=True)
    (ROOT/'generated/original-options-proof.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))


if __name__=='__main__':main()
