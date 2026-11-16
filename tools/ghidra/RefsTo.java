// List the references to the addresses given as script arguments, with the
// function each one sits in.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.*;

public class RefsTo extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; i++) {
                out.println("== refs to " + args[i]);
                for (Reference r : getReferencesTo(toAddr(Long.parseLong(args[i], 16)))) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    out.println("  " + r.getFromAddress() + " " + r.getReferenceType() + " in " + (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
                }
            }
        }
    }
}
