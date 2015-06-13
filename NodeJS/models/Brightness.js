module.exports = function(db, DataTypes) {
  return db.define('Brightness', {
    reading: DataTypes.INTEGER
  }, { updatedAt: false });
}
