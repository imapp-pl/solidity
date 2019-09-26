pragma experimental SMTChecker;

contract C {
	uint x = f(2);
	constructor () public {
		assert(x == 2);
	}

	function f(uint y) internal view returns (uint) {
		assert(y > 0);
		// This is reported as fail as a false positive for now,
		// but should be proven safe when function calls are implemented.
		assert(x == 0);
		return y;
	}
}
// ----
// Warning: (162-175): Assertion violation happens here
// Warning: (306-320): Assertion violation happens here
// Warning: (306-320): Assertion violation happens here
