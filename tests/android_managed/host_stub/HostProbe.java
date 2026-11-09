import android.app.Instrumentation;
import io.github.jichuo1.meguviz.memory.android.ManagedInstrumentation;
public final class HostProbe {
    public static void main(String[] args) {
        ManagedInstrumentation probe = new ManagedInstrumentation();
        if (args.length != 0) probe.onCreate(new android.os.Bundle() {
            @Override public String getString(String key) { return "probe_scope".equals(key) ? args[0] : null; }
        });
        probe.onStart();
        if (Instrumentation.result == null) { System.out.println("{\"host_status\":\"NO_REPORT\"}"); System.exit(1); }
        String report = Instrumentation.result.getString("report_json");
        System.out.println(report == null ? "{\"host_status\":\"NO_REPORT\"}" : report);
        if (Instrumentation.code != 0) System.exit(1);
    }
}
