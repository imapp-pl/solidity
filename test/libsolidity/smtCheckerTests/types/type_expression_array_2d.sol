pragma experimental SMTChecker;
contract C {

function f() public pure { int[][]; }

}
// ----
// Warning: (73-80): Statement has no effect.
// Warning: (73-78): Assertion checker does not yet implement this expression.
