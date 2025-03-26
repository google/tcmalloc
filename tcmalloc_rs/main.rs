use tcmalloc_rs;

#[global_allocator]
static GLOBAL: tcmalloc_rs::TCMalloc = tcmalloc_rs::TCMalloc;

fn main() {
    // This `Vec` will allocate memory through `GLOBAL` above
    println!("allocation a new vec");
    let mut v = Vec::new();
    println!("push an element");
    v.push(1);
    println!("done");
}