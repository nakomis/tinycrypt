from board.sexpr import Sym, parse, dumps, find, find_all


def test_parse_atoms_strings_and_nesting():
    node = parse('(pad "1" smd rect (at 0 -1.5 90) (layers "F.Cu" F.Paste))')
    assert node[0] == Sym("pad")
    assert node[1] == "1" and not isinstance(node[1], Sym)
    assert node[2] == Sym("smd")
    assert find(node, "at") == [Sym("at"), 0, -1.5, 90]
    assert find(node, "layers")[2] == Sym("F.Paste")


def test_escaped_quotes_round_trip():
    text = '(property "Value" "say \\"hi\\"")'
    assert dumps(parse(text)) == text


def test_numbers_keep_type():
    node = parse("(at 1 2.50 -3)")
    assert node[1] == 1 and isinstance(node[1], int)
    assert node[2] == 2.5 and isinstance(node[2], float)


def test_dumps_quotes_strings_not_symbols():
    assert dumps([Sym("net"), 3, "GND"]) == '(net 3 "GND")'


def test_find_all():
    node = parse('(fp (pad "1") (pad "2") (line))')
    assert [p[1] for p in find_all(node, "pad")] == ["1", "2"]
