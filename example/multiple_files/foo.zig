import "std.zig";

// purposefully conflicting function with main.zig
// but it's private so it should be OK
fn private_function() {
    %%stdout.printf("OK 1\n");
}

pub fn print_text() {
    private_function();
}
