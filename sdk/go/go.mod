module github.com/Pr1nted/Open-Doctrines/sdk/go

// 1.21 is the floor for unsafe.StringData / unsafe.SliceData, which is how the
// binding gets a pointer out of a string without copying. Nothing here is
// fetched: the example lives inside this module, so `tinygo build` resolves the
// import locally and never touches the network.
go 1.21
