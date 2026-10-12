// Recover code Ghidra's first pass missed: disassemble every direct call target in .text,
// create functions at those targets and at the start of orphaned instruction runs, re-analyse.
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import java.util.*;

public class RecoverCode extends GhidraScript {
    @Override
    public void run() throws Exception {
        MemoryBlock text = currentProgram.getMemory().getBlock(".text");
        Memory mem = currentProgram.getMemory();
        Listing l = currentProgram.getListing();
        int created = 0;
        for (int pass = 0; pass < 6; pass++) {
            // 1) targets of E8 rel32 calls whose source decodes as part of an instruction or undefined bytes
            Set<Address> targets = new TreeSet<>();
            byte[] buf = new byte[(int) text.getSize()];
            text.getBytes(text.getStart(), buf);
            long base = text.getStart().getOffset();
            for (int o = 0; o + 5 <= buf.length; o++) {
                if ((buf[o] & 0xff) != 0xe8) continue;
                int rel = (buf[o + 1] & 0xff) | (buf[o + 2] & 0xff) << 8 | (buf[o + 3] & 0xff) << 16 | (buf[o + 4] & 0xff) << 24;
                long t = base + o + 5 + rel;
                if (t < base || t >= base + buf.length) continue;
                Address src = toAddr(base + o), ta = toAddr(t);
                Instruction si = l.getInstructionAt(src);
                // only trust calls that are real instructions, or sit in undefined bytes
                if (si == null && l.getInstructionContaining(src) != null) continue;
                if (si != null && !si.getMnemonicString().equalsIgnoreCase("CALL")) continue;
                if (getFunctionAt(ta) == null && l.getInstructionContaining(ta) == null || (l.getInstructionAt(ta) != null && getFunctionAt(ta) == null))
                    targets.add(ta);
            }
            int before = created;
            for (Address ta : targets) {
                if (l.getInstructionAt(ta) == null) {
                    if (l.getInstructionContaining(ta) != null) continue;    // middle of an instruction
                    new DisassembleCommand(ta, null, true).applyTo(currentProgram, monitor);
                }
                if (l.getInstructionAt(ta) != null && getFunctionAt(ta) == null) {
                    if (new CreateFunctionCmd(ta).applyTo(currentProgram, monitor)) created++;
                }
            }
            // 2) orphaned instruction runs: create a function at each run start after padding
            for (Address a = text.getStart(); a.compareTo(text.getEnd()) < 0;) {
                Instruction ins = l.getInstructionAt(a);
                if (ins != null && getFunctionContaining(a) == null) {
                    Instruction prev = l.getInstructionBefore(a);
                    boolean runStart = prev == null || getFunctionContaining(prev.getAddress()) != null
                        || prev.getAddress().add(prev.getLength()).compareTo(a) != 0
                        || prev.getFlowType().isTerminal() || prev.getMnemonicString().equalsIgnoreCase("INT3");
                    if (runStart && new CreateFunctionCmd(a).applyTo(currentProgram, monitor)) created++;
                    a = a.add(ins.getLength());
                } else {
                    a = a.add(ins != null ? ins.getLength() : 1);
                }
            }
            println("pass " + pass + " created " + (created - before));
            if (created == before) break;
        }
        analyzeChanges(currentProgram);
        println("RECOVER created " + created + " functions, total " + currentProgram.getFunctionManager().getFunctionCount());
    }
}
