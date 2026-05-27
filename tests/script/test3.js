function makeCounter() {
    let x = 0;
    return function () {
      x = x + 1;
      return x;
    };
  }
  
  let c = makeCounter();
  console.log(c()); // 1
  console.log(c()); // 2
  console.log(c()); // 3