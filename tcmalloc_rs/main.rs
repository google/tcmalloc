use tcmalloc_rs;
use tcmalloc_extensions_rs::get_stats;

#[global_allocator]
static GLOBAL: tcmalloc_rs::TCMalloc = tcmalloc_rs::TCMalloc;

fn main() {
    // This `Vec` will allocate memory through `GLOBAL` above
    println!("allocation a new vec");
    let mut v = Vec::new();
    println!("push an element");
    v.push(1);
    println!("done");

    let stats = get_stats();
    println!("{}", stats)
}