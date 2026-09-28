import java.util.Random;
import java.util.SplittableRandom;

/**
 * Print one generator's words, one per line, lowercase hex.
 *
 * nextInt is java.util.Random. nextLong is SplittableRandom, which is
 * SplitMix64, not Random.nextLong.
 */
public class RandomOracle {
  public static void main(String[] args) {
    if (args.length != 3) {
      System.err.println("usage: RandomOracle nextInt|nextLong seed count");
      System.exit(2);
    }
    String which = args[0];
    long seed = Long.parseUnsignedLong(args[1]);
    int count = Integer.parseInt(args[2]);
    if (count < 0) {
      System.err.println("count must be non-negative");
      System.exit(2);
    }
    if (which.equals("nextInt")) {
      Random random = new Random(seed);
      for (int i = 0; i < count; i++) {
        System.out.printf("%08x%n", random.nextInt());
      }
      return;
    }
    if (which.equals("nextLong")) {
      SplittableRandom random = new SplittableRandom(seed);
      for (int i = 0; i < count; i++) {
        System.out.printf("%016x%n", random.nextLong());
      }
      return;
    }
    System.err.println("unknown draw " + which);
    System.exit(2);
  }
}
