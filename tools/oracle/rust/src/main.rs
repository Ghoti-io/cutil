// Print Xoshiro256PlusPlus::seed_from_u64, one lowercase hex word per line.
//
// This is rand's public generator, not SmallRng. SmallRng is the same step
// on a 64-bit host today and does not promise to stay that.

use rand::rngs::Xoshiro256PlusPlus;
use rand::{Rng, SeedableRng};

fn main() {
    let mut args = std::env::args().skip(1);
    let seed = args
        .next()
        .expect("usage: xoshiro-oracle seed count")
        .parse::<u64>()
        .expect("seed is not a decimal integer");
    let count = args
        .next()
        .expect("usage: xoshiro-oracle seed count")
        .parse::<u64>()
        .expect("count is not a decimal integer");
    if args.next().is_some() {
        panic!("usage: xoshiro-oracle seed count");
    }
    let mut rng = Xoshiro256PlusPlus::seed_from_u64(seed);
    for _ in 0..count {
        println!("{:016x}", rng.next_u64());
    }
}
