from collections.abc import Sequence


def main(argv: Sequence[str] | None = None) -> int:
	from .cli import main as cli_main

	return cli_main(argv)


__all__ = ["main"]