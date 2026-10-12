// Print stored stack-depth overrides and callee signatures for calls in the given functions. Args: <hex addr>...
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.function.CallDepthChangeInfo;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

public class DiagStack extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String a : getScriptArgs()) {
            Function f = getFunctionAt(toAddr(Long.parseLong(a, 16)));
            println("== " + f.getName(true) + " purge=" + f.getStackPurgeSize() + " cc=" + f.getCallingConventionName());
            for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
                if (!ins.getFlowType().isCall()) continue;
                Integer d = CallDepthChangeInfo.getStackDepthChange(currentProgram, ins.getAddress());
                String callee = "";
                for (Reference r : ins.getReferencesFrom()) {
                    Function c = getFunctionAt(r.getToAddress());
                    if (c != null) callee = c.getName(true) + " purge=" + c.getStackPurgeSize() + " cc=" + c.getCallingConventionName() + " thunk=" + c.isThunk();
                }
                println("  " + ins.getAddress() + " " + ins + (d != null ? " DEPTHOVERRIDE=" + d : "") + " " + callee);
            }
        }
    }
}
