// Create the game-thread function and list everything reachable from it through direct calls.
// Args: <hex entry> <out tsv>
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class Reach extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        Address entry = toAddr(Long.parseLong(a[0], 16));
        if (getFunctionAt(entry) == null) {
            disassemble(entry);
            new CreateFunctionCmd(entry).applyTo(currentProgram, monitor);
        }
        Set<Function> seen = new LinkedHashSet<>();
        Deque<Function> work = new ArrayDeque<>();
        work.add(getFunctionAt(entry));
        while (!work.isEmpty()) {
            Function f = work.poll();
            if (f == null || !seen.add(f)) continue;
            for (Function c : f.getCalledFunctions(monitor)) {
                if (c.isThunk()) c = c.getThunkedFunction(true);
                if (!c.isExternal() && !seen.contains(c)) work.add(c);
            }
            // function pointers stored in the function body (callbacks, tables)
            for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                for (Reference r : ins.getReferencesFrom()) {
                    if (r.getReferenceType().isData()) {
                        Function c = getFunctionAt(r.getToAddress());
                        if (c != null && !c.isExternal() && !seen.contains(c)) work.add(c);
                    }
                }
            }
        }
        PrintWriter out = new PrintWriter(new FileWriter(a[1]));
        for (Function f : seen) out.println(f.getEntryPoint() + "\t" + f.getName(true) + "\t" + f.getBody().getNumAddresses());
        out.close();
        println("REACHABLE " + seen.size());
    }
}
