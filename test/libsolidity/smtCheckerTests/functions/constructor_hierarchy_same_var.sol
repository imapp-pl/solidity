pragma experimental SMTChecker;
contract C {
	uint a;
	constructor(uint x) public {
		a = x;
	}
}

contract A is C {
	uint a;
	constructor() C(2) public {
		assert(a == 0);
		assert(C.a == 0);
	}
}
// ----
// Warning: (175-191): Assertion violation happens here
