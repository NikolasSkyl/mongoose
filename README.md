# mongoose

<a href="https://nikolasskyl.github.io/mongoose/"><img src="https://img.shields.io/badge/Docs-Read-2F81F7?style=for-the-badge" alt="Docs - Read"/></a>
<img src="https://img.shields.io/badge/Donna-mongoose-FF6347?style=for-the-badge" alt="Donna - mongoose"/>
<img src="https://img.shields.io/github/actions/workflow/status/NikolasSkyl/mongoose/test.yml?branch=main&label=Test&style=for-the-badge" alt="Test status"/>

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
