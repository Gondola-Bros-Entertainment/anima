# Anima Pipeline

Optional offline asset authoring tools for the
[Anima C++ engine](https://github.com/Gondola-Bros-Entertainment/anima).
The engine runtime does not require this package or Blender.

The pure-Python modules validate recipes, separate GLB resources, compare exported
geometry and record build receipts. Blender adapters export animation, independent
props and fitted meshes from caller-supplied content. They do not supply game
assets or inventory rules. The current export workflow requires 30 Hz recipes;
see the [authoring guide](https://github.com/Gondola-Bros-Entertainment/anima/blob/main/docs/authoring.md)
for supported formats, Blender requirements and examples.

Requires Python 3.10+. From an Anima checkout, install with:

```sh
python -m pip install ./python
```

Licensed under Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
