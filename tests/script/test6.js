function a() {
    let x = 1;
    function b() {
      let y = 2;
      function c() {
        return x + y;
      }
      return c;
    }
    return b();
  }
  
  let f = a();
  console.log(f());
  // output: 3
  