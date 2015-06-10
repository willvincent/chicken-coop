module.exports = function(app, express) {
  var router = express.Router();


  router.get('/temp', function (req, res) {
    res.json({ message: "Hello from temp!" });
  });

  router.get('/light', function (req, res) {
    res.json({ message: "Hello from light!" });
  });

  router.stack.forEach(function(item) { 
    var methods = [];
    Object.keys(item.route.methods).forEach(function(method) {
      if (item.route.methods[method] == true) {
        methods.push(method.toUpperCase());
      }
    });
    console.log('  /api' + item.route.path + ' [' + methods.toString() + ']'); 
  });
  app.use('/api', router)
};
