// How much of .text is instructions inside functions, instructions outside functions, or undefined.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;

public class Coverage extends GhidraScript {
    @Override
    public void run() throws Exception {
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        Listing l = currentProgram.getListing();
        long inFunc = 0, outFunc = 0, undef = 0;
        long[] undefByRegion = new long[16];
        for (Address a = text.getStart(); a.compareTo(text.getEnd()) <= 0;) {
            CodeUnit cu = l.getCodeUnitAt(a);
            int len = cu != null ? cu.getLength() : 1;
            if (cu instanceof Instruction) {
                if (getFunctionContaining(a) != null) inFunc += len; else outFunc += len;
            } else {
                // skip padding (int3 / nop / zero)
                byte b = currentProgram.getMemory().getByte(a);
                if (b != (byte) 0xcc && b != (byte) 0x90 && b != 0) {
                    undef += len;
                    undefByRegion[(int) ((a.getOffset() >> 16) & 0xf)] += len;
                }
            }
            a = a.add(len);
        }
        println("COVERAGE inFunc=" + inFunc + " outFunc=" + outFunc + " undefined(non-padding)=" + undef);
        StringBuilder sb = new StringBuilder("UNDEF by 64K:");
        for (int i = 0; i < 16; i++) if (undefByRegion[i] > 0) sb.append(String.format(" 0x4%x0000=%d", i, undefByRegion[i]));
        println(sb.toString());
    }
}
