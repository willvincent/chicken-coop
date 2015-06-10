module.exports = function(db, DataTypes) {
  return db.define('Brightness', {
    reading: DataTypes.FLOAT
  });
}
