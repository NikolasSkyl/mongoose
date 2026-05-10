# mongoose

Static file server for Donna, backed by the C Mongoose library.

```donna
import mongoose

pub fn main() -> Int:
  mongoose.serve("docs", 1313)
```

Run checks:

```sh
donna check
donna test
```
