#!/usr/bin/env python3
"""Minimal Motorola 6809 disassembler for the Vectrex BIOS (rom.dat @ 0xE000).
Used to scope BIOS draw routines for HLE. Not a full validator -- good enough
to read control flow and operands of the hot draw/timing routines."""
import sys

ROM_BASE = 0xE000

# (mnemonic, mode) tables. modes:
#  inh, imm8, imm16, dir, ext, idx, rel8, rel16, exg(reg postbyte), psh/pul(reg list)
INH='inh'; I8='imm8'; I16='imm16'; DIR='dir'; EXT='ext'; IDX='idx'
R8='rel8'; R16='rel16'; RGS='regs'; PSH='push'; PUL='pull'

p0 = {
 0x00:('NEG',DIR),0x03:('COM',DIR),0x04:('LSR',DIR),0x06:('ROR',DIR),0x07:('ASR',DIR),
 0x08:('LSL',DIR),0x09:('ROL',DIR),0x0A:('DEC',DIR),0x0C:('INC',DIR),0x0D:('TST',DIR),
 0x0E:('JMP',DIR),0x0F:('CLR',DIR),
 0x12:('NOP',INH),0x13:('SYNC',INH),0x16:('LBRA',R16),0x17:('LBSR',R16),0x19:('DAA',INH),
 0x1A:('ORCC',I8),0x1C:('ANDCC',I8),0x1D:('SEX',INH),0x1E:('EXG',RGS),0x1F:('TFR',RGS),
 0x20:('BRA',R8),0x21:('BRN',R8),0x22:('BHI',R8),0x23:('BLS',R8),0x24:('BCC',R8),
 0x25:('BCS',R8),0x26:('BNE',R8),0x27:('BEQ',R8),0x28:('BVC',R8),0x29:('BVS',R8),
 0x2A:('BPL',R8),0x2B:('BMI',R8),0x2C:('BGE',R8),0x2D:('BLT',R8),0x2E:('BGT',R8),0x2F:('BLE',R8),
 0x30:('LEAX',IDX),0x31:('LEAY',IDX),0x32:('LEAS',IDX),0x33:('LEAU',IDX),
 0x34:('PSHS',PSH),0x35:('PULS',PUL),0x36:('PSHU',PSH),0x37:('PULU',PUL),
 0x39:('RTS',INH),0x3A:('ABX',INH),0x3B:('RTI',INH),0x3C:('CWAI',I8),0x3D:('MUL',INH),0x3F:('SWI',INH),
 0x40:('NEGA',INH),0x43:('COMA',INH),0x44:('LSRA',INH),0x46:('RORA',INH),0x47:('ASRA',INH),
 0x48:('LSLA',INH),0x49:('ROLA',INH),0x4A:('DECA',INH),0x4C:('INCA',INH),0x4D:('TSTA',INH),0x4F:('CLRA',INH),
 0x50:('NEGB',INH),0x53:('COMB',INH),0x54:('LSRB',INH),0x56:('RORB',INH),0x57:('ASRB',INH),
 0x58:('LSLB',INH),0x59:('ROLB',INH),0x5A:('DECB',INH),0x5C:('INCB',INH),0x5D:('TSTB',INH),0x5F:('CLRB',INH),
 0x60:('NEG',IDX),0x63:('COM',IDX),0x64:('LSR',IDX),0x66:('ROR',IDX),0x67:('ASR',IDX),
 0x68:('LSL',IDX),0x69:('ROL',IDX),0x6A:('DEC',IDX),0x6C:('INC',IDX),0x6D:('TST',IDX),
 0x6E:('JMP',IDX),0x6F:('CLR',IDX),
 0x70:('NEG',EXT),0x73:('COM',EXT),0x74:('LSR',EXT),0x76:('ROR',EXT),0x77:('ASR',EXT),
 0x78:('LSL',EXT),0x79:('ROL',EXT),0x7A:('DEC',EXT),0x7C:('INC',EXT),0x7D:('TST',EXT),
 0x7E:('JMP',EXT),0x7F:('CLR',EXT),
 0x80:('SUBA',I8),0x81:('CMPA',I8),0x82:('SBCA',I8),0x83:('SUBD',I16),0x84:('ANDA',I8),
 0x85:('BITA',I8),0x86:('LDA',I8),0x88:('EORA',I8),0x89:('ADCA',I8),0x8A:('ORA',I8),
 0x8B:('ADDA',I8),0x8C:('CMPX',I16),0x8D:('BSR',R8),0x8E:('LDX',I16),
 0x90:('SUBA',DIR),0x91:('CMPA',DIR),0x92:('SBCA',DIR),0x93:('SUBD',DIR),0x94:('ANDA',DIR),
 0x95:('BITA',DIR),0x96:('LDA',DIR),0x97:('STA',DIR),0x98:('EORA',DIR),0x99:('ADCA',DIR),
 0x9A:('ORA',DIR),0x9B:('ADDA',DIR),0x9C:('CMPX',DIR),0x9D:('JSR',DIR),0x9E:('LDX',DIR),0x9F:('STX',DIR),
 0xA0:('SUBA',IDX),0xA1:('CMPA',IDX),0xA2:('SBCA',IDX),0xA3:('SUBD',IDX),0xA4:('ANDA',IDX),
 0xA5:('BITA',IDX),0xA6:('LDA',IDX),0xA7:('STA',IDX),0xA8:('EORA',IDX),0xA9:('ADCA',IDX),
 0xAA:('ORA',IDX),0xAB:('ADDA',IDX),0xAC:('CMPX',IDX),0xAD:('JSR',IDX),0xAE:('LDX',IDX),0xAF:('STX',IDX),
 0xB0:('SUBA',EXT),0xB1:('CMPA',EXT),0xB2:('SBCA',EXT),0xB3:('SUBD',EXT),0xB4:('ANDA',EXT),
 0xB5:('BITA',EXT),0xB6:('LDA',EXT),0xB7:('STA',EXT),0xB8:('EORA',EXT),0xB9:('ADCA',EXT),
 0xBA:('ORA',EXT),0xBB:('ADDA',EXT),0xBC:('CMPX',EXT),0xBD:('JSR',EXT),0xBE:('LDX',EXT),0xBF:('STX',EXT),
 0xC0:('SUBB',I8),0xC1:('CMPB',I8),0xC2:('SBCB',I8),0xC3:('ADDD',I16),0xC4:('ANDB',I8),
 0xC5:('BITB',I8),0xC6:('LDB',I8),0xC8:('EORB',I8),0xC9:('ADCB',I8),0xCA:('ORB',I8),
 0xCB:('ADDB',I8),0xCC:('LDD',I16),0xCE:('LDU',I16),
 0xD0:('SUBB',DIR),0xD1:('CMPB',DIR),0xD2:('SBCB',DIR),0xD3:('ADDD',DIR),0xD4:('ANDB',DIR),
 0xD5:('BITB',DIR),0xD6:('LDB',DIR),0xD7:('STB',DIR),0xD8:('EORB',DIR),0xD9:('ADCB',DIR),
 0xDA:('ORB',DIR),0xDB:('ADDB',DIR),0xDC:('LDD',DIR),0xDD:('STD',DIR),0xDE:('LDU',DIR),0xDF:('STU',DIR),
 0xE0:('SUBB',IDX),0xE1:('CMPB',IDX),0xE2:('SBCB',IDX),0xE3:('ADDD',IDX),0xE4:('ANDB',IDX),
 0xE5:('BITB',IDX),0xE6:('LDB',IDX),0xE7:('STB',IDX),0xE8:('EORB',IDX),0xE9:('ADCB',IDX),
 0xEA:('ORB',IDX),0xEB:('ADDB',IDX),0xEC:('LDD',IDX),0xED:('STD',IDX),0xEE:('LDU',IDX),0xEF:('STU',IDX),
 0xF0:('SUBB',EXT),0xF1:('CMPB',EXT),0xF2:('SBCB',EXT),0xF3:('ADDD',EXT),0xF4:('ANDB',EXT),
 0xF5:('BITB',EXT),0xF6:('LDB',EXT),0xF7:('STB',EXT),0xF8:('EORB',EXT),0xF9:('ADCB',EXT),
 0xFA:('ORB',EXT),0xFB:('ADDB',EXT),0xFC:('LDD',EXT),0xFD:('STD',EXT),0xFE:('LDU',EXT),0xFF:('STU',EXT),
}
p2 = {
 0x21:('LBRN',R16),0x22:('LBHI',R16),0x23:('LBLS',R16),0x24:('LBCC',R16),0x25:('LBCS',R16),
 0x26:('LBNE',R16),0x27:('LBEQ',R16),0x28:('LBVC',R16),0x29:('LBVS',R16),0x2A:('LBPL',R16),
 0x2B:('LBMI',R16),0x2C:('LBGE',R16),0x2D:('LBLT',R16),0x2E:('LBGT',R16),0x2F:('LBLE',R16),
 0x3F:('SWI2',INH),
 0x83:('CMPD',I16),0x8C:('CMPY',I16),0x8E:('LDY',I16),
 0x93:('CMPD',DIR),0x9C:('CMPY',DIR),0x9E:('LDY',DIR),0x9F:('STY',DIR),
 0xA3:('CMPD',IDX),0xAC:('CMPY',IDX),0xAE:('LDY',IDX),0xAF:('STY',IDX),
 0xB3:('CMPD',EXT),0xBC:('CMPY',EXT),0xBE:('LDY',EXT),0xBF:('STY',EXT),
 0xCE:('LDS',I16),0xDE:('LDS',DIR),0xDF:('STS',DIR),0xEE:('LDS',IDX),0xEF:('STS',IDX),
 0xFE:('LDS',EXT),0xFF:('STS',EXT),
}
p3 = {
 0x3F:('SWI3',INH),0x83:('CMPU',I16),0x8C:('CMPS',I16),
 0x93:('CMPU',DIR),0x9C:('CMPS',DIR),0xA3:('CMPU',IDX),0xAC:('CMPS',IDX),
 0xB3:('CMPU',EXT),0xBC:('CMPS',EXT),
}
IXREG=['X','Y','U','S']
TFRREG={0:'D',1:'X',2:'Y',3:'U',4:'S',5:'PC',8:'A',9:'B',10:'CC',11:'DP'}
PSHREG=['CC','A','B','DP','X','Y','U','PC']  # bit0..bit7 (U/S swap handled by op)

def s8(v): return v-256 if v&0x80 else v
def s16(v): return v-65536 if v&0x8000 else v

def idx_operand(rom, p):
    pb = rom[p]; p+=1
    r = IXREG[(pb>>5)&3]
    if not (pb&0x80):
        off = pb & 0x1F
        if off & 0x10: off -= 0x20
        return f"{off},{r}", p
    ind = pb & 0x10
    m = pb & 0x0F
    def wrap(s): return f"[{s}]" if ind else s
    if m==0x0: return f",{r}+", p
    if m==0x1: return f",{r}++", p
    if m==0x2: return f",-{r}", p
    if m==0x3: return f",--{r}", p
    if m==0x4: return wrap(f",{r}"), p
    if m==0x5: return wrap(f"B,{r}"), p
    if m==0x6: return wrap(f"A,{r}"), p
    if m==0x8:
        n=s8(rom[p]); p+=1; return wrap(f"{n},{r}"), p
    if m==0x9:
        n=s16((rom[p]<<8)|rom[p+1]); p+=2; return wrap(f"{n},{r}"), p
    if m==0xB: return wrap(f"D,{r}"), p
    if m==0xC:
        n=s8(rom[p]); p+=1; return wrap(f"{n},PCR"), p
    if m==0xD:
        n=s16((rom[p]<<8)|rom[p+1]); p+=2; return wrap(f"{n},PCR"), p
    if m==0xF:
        a=(rom[p]<<8)|rom[p+1]; p+=2; return f"[${a:04X}]", p
    return f"?pb{pb:02X}", p

def reglist(b):
    return ','.join(n for i,n in enumerate(PSHREG) if b&(1<<i))

def disasm_one(rom, addr):
    p = addr - ROM_BASE
    start = p
    op = rom[p]; p+=1
    if op==0x10: tbl=p2; op=rom[p]; p+=1
    elif op==0x11: tbl=p3; op=rom[p]; p+=1
    else: tbl=p0
    e = tbl.get(op)
    if not e:
        return addr, f".byte ${rom[start]:02X}", addr+1
    mn, mode = e
    txt=mn
    if mode==INH: pass
    elif mode==I8: txt=f"{mn} #${rom[p]:02X}"; p+=1
    elif mode==I16: txt=f"{mn} #${(rom[p]<<8)|rom[p+1]:04X}"; p+=2
    elif mode==DIR: txt=f"{mn} <${rom[p]:02X}"; p+=1
    elif mode==EXT: txt=f"{mn} ${(rom[p]<<8)|rom[p+1]:04X}"; p+=2
    elif mode==IDX:
        o,p = idx_operand(rom,p); txt=f"{mn} {o}"
    elif mode==R8:
        d=s8(rom[p]); p+=1; txt=f"{mn} ${(addr+(p-start)+d)&0xFFFF:04X}"
    elif mode==R16:
        d=s16((rom[p]<<8)|rom[p+1]); p+=2; txt=f"{mn} ${(addr+(p-start)+d)&0xFFFF:04X}"
    elif mode==RGS:
        pb=rom[p]; p+=1
        txt=f"{mn} {TFRREG.get(pb>>4,'?')},{TFRREG.get(pb&0xF,'?')}"
    elif mode in (PSH,PUL):
        pb=rom[p]; p+=1; txt=f"{mn} {reglist(pb)}"
    return addr, txt, addr+(p-start)

def main():
    rom = open('Source/rom.dat','rb').read()
    # ranges: start,end (hex addresses)
    ranges = [(int(a,16),int(b,16)) for a,b in (x.split(':') for x in sys.argv[1:])]
    if not ranges:
        ranges=[(0xF300,0xF360)]
    for (a,b) in ranges:
        print(f"==== {a:04X}..{b:04X} ====")
        addr=a
        while addr < b:
            ad,txt,nxt = disasm_one(rom, addr)
            raw=' '.join(f"{rom[ad-ROM_BASE+i]:02X}" for i in range(nxt-ad))
            print(f"{ad:04X}: {raw:<14} {txt}")
            addr=nxt

if __name__=='__main__':
    main()
