// adb-sysmon's battery current: BatteryManager's CURRENT_NOW (property 2), read
// through the batteryproperties service, since sysfs's power_supply is closed to
// the shell user. Run on the device as the shell user:
//   CLASSPATH=/data/local/tmp/sysmon/pw.dex app_process / Pw
// Reflection only, so it builds without android.jar:
//   javac --release 8 Pw.java && d8 --min-api 1 --output . Pw.class && mv classes.dex sysmon-pw.dex
// (Reconstructed 2026-11-21 from sysmon-pw.dex, whose source was lost with
// ~/.cache/sysmon; it reads the same property. That dex is the one devices run.)
public class Pw {
  public static void main(String[] a) throws Exception {
    Object b = Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, "batteryproperties");
    Object r = Class.forName("android.os.IBatteryPropertiesRegistrar$Stub").getMethod("asInterface", Class.forName("android.os.IBinder")).invoke(null, b);
    Class<?> pc = Class.forName("android.os.BatteryProperty");
    Object p = pc.getConstructor().newInstance();
    r.getClass().getMethod("getProperty", Integer.TYPE, pc).invoke(r, 2, p);
    System.out.println(pc.getMethod("getLong").invoke(p));
  }
}
