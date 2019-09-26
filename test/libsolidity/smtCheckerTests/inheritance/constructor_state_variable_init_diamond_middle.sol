pragma experimental SMTChecker;

contract A {
	uint x = 1;
}

contract B is A {
	uint x = 2;
}

contract C is A {
	uint x = 3;
}

contract D is B, C {
	constructor() public {
		assert(x == 3);
		assert(x == 4);
	}
}
// ----
// Warning: (195-209): Assertion violation happens here
