pragma experimental SMTChecker;

contract A {
	uint x = 1;
}

contract B is A {
	uint x = 2;
}

contract C is B {
	uint x = 3;
}

contract D is C {
	constructor() public {
		assert(x == 3);
		assert(x == 2);
	}
}
// ----
// Warning: (192-206): Assertion violation happens here
