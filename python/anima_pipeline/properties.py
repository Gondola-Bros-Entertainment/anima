"""Explicit authoring property names; consumers own their content namespace."""

from dataclasses import dataclass, fields


@dataclass(frozen=True)
class AuthoringProperties:
    sampling_defaults: str = "anima.action_sampling_defaults"
    reference_speed: str = "anima.reference_speed"
    render_group: str = "anima.render_group"
    semantic_parent: str = "anima.semantic_parent"
    generated_from: str = "anima.generated_from"
    motion_node: str = "anima.motion_node"
    contact_space: str = "anima.contact_space"

    def __post_init__(self):
        values = [getattr(self, field.name) for field in fields(self)]
        if any(not isinstance(value, str) or not value for value in values) or len(
            set(values)
        ) != len(values):
            raise ValueError("Authoring properties must have distinct nonempty names")
