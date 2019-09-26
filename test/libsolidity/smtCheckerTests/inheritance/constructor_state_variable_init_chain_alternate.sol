pragma experimental SMTChecker;

contract A {
	uint x = 1;
}

contract B is A {
	uint x = 2;
}

contract C is B {
}

contract D is C {
	constructor() public {
		assert(x == 2);
		assert(x == 3);
	}
}
// ----
// Warning: (179-193): Assertion violation happens here
