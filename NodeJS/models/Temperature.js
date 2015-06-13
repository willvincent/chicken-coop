module.exports = function(db, DataTypes) {
  return db.define('Temperature', {
    reading: DataTypes.FLOAT
  }, { updatedAt: false });
}
