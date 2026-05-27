let f;

function outer() {
  let secret = 42;
  f = function () {
    return secret;
  };
}

outer();
console.log(f());
